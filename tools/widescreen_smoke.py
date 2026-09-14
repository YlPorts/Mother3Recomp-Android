#!/usr/bin/env python3
"""Compare native and widescreen gameplay from a user-owned savestate.

Stages separate executables, mod selections and saves under --output. Drives
identical controller input through TCP, checks the center every eight frames,
and compares guest RAM/VRAM at checkpoints. No original save is written.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import zlib


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.file = self.socket.makefile("rb")

    def call(self, cmd, **kwargs):
        self.socket.sendall((json.dumps(dict(cmd=cmd, **kwargs)) + "\n").encode())
        response = json.loads(self.file.readline())
        if not response.get("ok"):
            raise RuntimeError(response)
        return response

    def close(self):
        self.file.close()
        self.socket.close()


def png(path, data, width, height):
    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload))
    raw = b"".join(b"\0" + data[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def launch(args, name, enabled):
    root = args.output / name
    root.mkdir(parents=True, exist_ok=True)
    executable = root / args.exe.name
    shutil.copy2(args.exe, executable)
    for dll in args.exe.parent.glob("*.dll"):
        shutil.copy2(dll, root / dll.name)
    catalog = Path(__file__).resolve().parents[1] / "mods" / "preloaded"
    shutil.copytree(catalog, root / "mods", dirs_exist_ok=True)
    (root / "mods" / "state.toml").write_text(f'''format_version = 1
[[package]]
id = "pokemon-emerald.enhancement.widescreen"
version = "0.1.0"
[[feature]]
package_id = "pokemon-emerald.enhancement.widescreen"
id = "widescreen"
enabled = {str(enabled).lower()}
[feature.values]
aspect = "{args.aspect}"
''')
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("GBARECOMP_"):
            del env[key]
    env.update(GBARECOMP_STRICT_STATIC="1", RECOMP_RTC_EPOCH="1789261200")
    if args.toolchain:
        env["PATH"] = str(args.toolchain) + os.pathsep + env["PATH"]
    # A stale display request must not bypass the disabled plugin in native.
    if not enabled:
        env["GBARECOMP_VIEW_WIDTH"] = "569"
    port = free_port()
    log = (root / "run.log").open("w")
    process = subprocess.Popen([str(executable), "--bios", str(args.bios),
        "--rom", str(args.rom), "--save-path", str(root / "test.sav"),
        "--no-window", "--tcp", str(port)], cwd=root, env=env,
        stdout=log, stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    deadline = time.monotonic() + 20
    try:
        while True:
            if process.poll() is not None:
                raise RuntimeError(f"{name} exited early: {process.returncode}")
            try:
                client = Client(port)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        client.call("savestate_load", path=str(args.state))
        return root, process, log, client
    except BaseException:
        process.terminate()
        process.wait(timeout=10)
        log.close()
        raise


def read_region(client, region, base, size):
    return b"".join(bytes.fromhex(client.call("read_" + region,
        addr=hex(base+off), len=min(8192, size-off))["data"])
        for off in range(0, size, 8192))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "bios", "rom", "state", "output"):
        p.add_argument("--" + name, type=lambda s: Path(s).resolve(), required=True)
    p.add_argument("--toolchain", type=Path)
    p.add_argument("--aspect", choices=["fit", "16:9", "21:9", "32:9"], default="32:9")
    p.add_argument("--frames", type=int, default=600)
    p.add_argument("--idle", action="store_true", help="Do not drive the walking route")
    p.add_argument("--route", choices=["walk", "left", "right", "left-right"], default="walk")
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    runs = []
    report = dict(aspect=args.aspect, frames=args.frames, screenshots=0, memory_checks=0,
                  center_different_channels=0, coverage=[])
    try:
        runs.append(launch(args, "native", False))
        runs.append(launch(args, "wide", True))
        clients = [r[3] for r in runs]
        expected_width = {"fit":284, "16:9":284, "21:9":373, "32:9":569}[args.aspect]
        route = [(0, 1023), (60, 1007), (180, 1023), (200, 991),
                 (320, 1023), (340, 959), (460, 1023), (480, 895)]
        if args.route == "left": route = [(0, 991)]
        if args.route == "right": route = [(0, 1007)]
        if args.route == "left-right": route = [(0, 991), (180, 1007), (580, 991)]
        route = dict([(0, 1023)] if args.idle else route)
        for frame in range(args.frames):
            if frame in route:
                for c in clients:
                    c.call("set_keyinput", value=route[frame])
            for c in clients:
                c.call("step")
            if frame < 2:  # A loaded native framebuffer needs a complete frame.
                continue
            if frame % 8 == 0 or frame == args.frames-1:
                shots = [c.call("screenshot") for c in clients]
                if [s["w"] for s in shots] != [240, expected_width]:
                    raise AssertionError("plugin width or disabled-mode capability gate failed")
                raw = [bytes.fromhex(s["data"]) for s in shots]
                left = (expected_width - 240)//2
                center = b"".join(raw[1][(y*expected_width+left)*3:(y*expected_width+left+240)*3]
                                  for y in range(160))
                diff = sum(a != b for a,b in zip(raw[0], center))
                report["center_different_channels"] += diff
                report["screenshots"] += 1
                if diff or frame == 8 or frame % 120 == 0 or frame == args.frames-1:
                    for i,name in enumerate(["native", "wide"]):
                        png(args.output/f"{name}-{frame:04d}.png", raw[i], shots[i]["w"], 160)
                if diff:
                    raise AssertionError(f"native center differs at frame {frame}: {diff} channels")
            if frame in (60, args.frames-1):
                for region, base, size in [("ewram",0x02000000,0x40000),("iwram",0x03000000,0x8000),
                                           ("vram",0x06000000,0x18000),("pal",0x05000000,0x400),("oam",0x07000000,0x400)]:
                    data = [read_region(c, region, base, size) for c in clients]
                    if data[0] != data[1]:
                        raise AssertionError(f"guest {region} changed at frame {frame}")
                    report["memory_checks"] += 1
                    if frame == args.frames-1:
                        (runs[1][0]/(region+".bin")).write_bytes(data[1])
        for c in clients:
            coverage = c.call("misses")
            if coverage["distinct_misses"] or coverage["interpreted_insns"] or coverage["healed_native"]:
                raise AssertionError("strict static coverage failed")
            report["coverage"].append(coverage["coverage"])
        clients[1].call("savestate_save", path=str(runs[1][0]/"final.state"))
        (runs[1][0]/"io.bin").write_bytes(read_region(clients[1], "io", 0x04000000, 0x400))
    finally:
        for root, process, log, client in runs:
            try:
                client.call("quit")
                client.close()
                process.wait(timeout=20)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                log.close()
        (args.output/"report.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
