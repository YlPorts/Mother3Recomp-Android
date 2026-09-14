#include "emerald_extended_view.h"
#include "emerald_object_view.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <memory>

using emerald::ViewStatus;
void require(bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

struct Fixture {
    std::vector<std::uint8_t> ewram = std::vector<std::uint8_t>(0x40000);
    std::vector<std::uint8_t> iwram = std::vector<std::uint8_t>(0x8000);
    std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(0x10000);
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(0x18000);
    std::vector<std::uint8_t> io = std::vector<std::uint8_t>(0x400);
    emerald::ViewMemory memory() const {
        return {ewram.data(), iwram.data(), rom.data(), rom.size(), vram.data(), io.data()};
    }
    void word(std::uint32_t addr, unsigned value) {
        auto* p = const_cast<std::uint8_t*>(memory().bytes(addr, 2));
        require(p != nullptr, "bad fixture address");
        p[0] = value; p[1] = value >> 8;
    }
    void dword(std::uint32_t addr, unsigned value) { word(addr, value); word(addr + 2, value >> 16); }
    void reg(int addr, unsigned value) { io[addr] = value; io[addr+1] = value >> 8; }
    Fixture() {
        dword(0x030022c4, 0x08085e5d);
        dword(0x02037318, 0x08000100);
        dword(0x08000108, 0x08000200); // border
        dword(0x08000110, 0x08000300); // primary
        dword(0x08000114, 0x08000318); // secondary
        dword(0x0800030c, 0x08001000);
        dword(0x08000310, 0x08005000);
        dword(0x08000324, 0x08003000);
        dword(0x08000328, 0x08005400);
        dword(0x03005dc0, 40);
        dword(0x03005dc4, 32);
        dword(0x03005dc8, 0x02000000);
        dword(0x03005d8c, 0x02010000);
        word(0x02010000, 10); word(0x02010002, 5);
        // Unique metatile per column. Explicit NORMAL layout: BG3 garbage,
        // BG2 bottom four tiles, BG1 upper four (including flip/palette bits).
        for (int id = 0; id < 512; ++id) {
            for (int q = 0; q < 8; ++q) word(0x08001000 + id*16 + q*2, 0x1400 + id*8 + q);
        }
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 40; ++x)
            word(0x02000000 + (y*40+x)*2, x);
        reg(0, 0x0e00);
        for (int bg = 1; bg <= 3; ++bg) reg(8 + bg*2, (27+bg)*256 + bg);
        ring();
    }
    void ring(int origin_x = 20, int origin_y = 10, int rx = 0, int ry = 0) {
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const int wx = origin_x + x, wy = origin_y + y;
            const int id = wx / 2, q = (wx & 1) + (wy & 1)*2;
            const unsigned entries[] = {unsigned(0x1400+id*8+q+4), unsigned(0x1400+id*8+q), 0x3014};
            for (int bg = 0; bg < 3; ++bg) {
                int addr = (28+bg)*0x800 + (((y+ry)&31)*32+((x+rx)&31))*2;
                vram[addr] = entries[bg]; vram[addr+1] = entries[bg] >> 8;
            }
        }
    }
};

int main() {
    // An NPC straddling the native edge, with one 16x32 subsprite. The live
    // guest OAM is the validation source; then exercise the software culler.
    {
        Fixture npc;
        std::vector<std::uint8_t> oam(0x400), pal(0x400);
        auto memory = npc.memory(); memory.oam = oam.data(); memory.pal = pal.data();
        npc.reg(0, 0x1e40);
        npc.ewram[0x37350] = 1;
        const unsigned sprite = 0x02020630;
        npc.word(sprite, 0x8000); npc.word(sprite+2, 0x81f0); npc.word(sprite+4, 0x0800);
        npc.word(sprite+32, -8); npc.word(sprite+34, 16);
        npc.word(sprite+40, 0xf0f8); // center-to-corner (-8,-16)
        npc.word(sprite+62, 3); npc.word(sprite+66, 0x8041);
        npc.dword(sprite+24, 0x08006000);
        npc.word(0x08006008, 1); npc.dword(0x0800600c, 0x08006018);
        npc.word(0x08006018, 0xf0f8); npc.word(0x0800601a, 0x800a);
        std::copy_n(npc.ewram.data()+0x20630, 6, oam.data());
        std::fill_n(npc.vram.begin()+0x10000, 256, 0x11);
        pal[0x202] = 31;
        emerald::FieldView field;
        // Large host framebuffer stays off the small Windows stack.
        auto objects = std::make_unique<emerald::ObjectView>();
        require(field.prepare(memory, 569) == ViewStatus::Ready, "NPC fixture field failed");
        require(objects->prepare(memory, field, 569) && objects->verified_parts() == 1,
                "visible NPC failed OAM validation");
        int left, width;
        const auto* row = objects->row(8, &left, &width);
        require(row && row[-16-left].color == 31 && row[-16-left].priority == 2,
                "NPC lost its margin pixels or subsprite priority");
        require(row[-left].color == 0x8000, "NPC shadow wrote into native center");
        // Next-frame Sprite.oam may lead hardware by a pixel and a turn.
        npc.word(sprite+2, 0x91f1);
        require(objects->prepare(memory, field, 569), "bounded OAM publish lag rejected");
        row = objects->row(8,&left,&width);
        require(row[-16-left].color == 31 && row[-left].color == 0x8000,
                "next-frame coordinates replaced published NPC position");
        npc.word(sprite+2, 0x81f0);
        // Recycled/mismatched OAM must fail closed immediately.
        oam[4] ^= 1;
        require(!objects->prepare(memory, field, 569) && !objects->row(8,&left,&width),
                "stale OAM did not invalidate NPC shadow");
        npc.ewram[0x37351] = 0x40; // offScreen, NOT script-invisible
        npc.ewram[0x20630+62] = 7;
        npc.word(sprite+32, -40);
        require(objects->prepare(memory, field, 569), "software-culled live NPC rejected");
        row = objects->row(8,&left,&width);
        require(row[-48-left].color == 31, "software-culled NPC disappeared");
        npc.ewram[0x37351] |= 0x20;
        require(objects->prepare(memory, field, 569), "script-hidden NPC fixture failed");
        row = objects->row(8,&left,&width);
        require(row[-48-left].color == 0x8000, "script-hidden NPC resurrected");
        npc.ewram[0x37350] = 0;
        require(objects->prepare(memory, field, 569) && objects->objects() == 0,
                "removed object left a stale sprite");
        field.prepare(memory, 240);
        require(!objects->prepare(memory, field, 569) && !objects->row(8,&left,&width),
                "NPC layer survived native/non-field reset");
    }
    Fixture f;
    emerald::FieldView view;
    require(view.prepare(f.memory(), 240) == ViewStatus::Native, "native mode not inert");
    const auto ewram = f.ewram, iwram = f.iwram, rom = f.rom, vram = f.vram, io = f.io;
    for (int width : {241, 284, 373, 569}) {
        require(view.prepare(f.memory(), width) == ViewStatus::Ready, "field mapping rejected");
        std::uint16_t tile = 0;
        require(view.tile(1, 240, 0, &tile) && tile == 0x14cc, "right margin wrapped instead of showing unseen map");
        require(!view.tile(0, 240, 0, &tile), "UI background repeated into margins");
        require(!view.tile(1, 5000, 0, &tile) && !view.tile(1, 240, 160, &tile), "output bounds not enforced");
    }
    require(f.ewram == ewram && f.iwram == iwram && f.rom == rom && f.vram == vram && f.io == io,
            "renderer mutated guest state");
    // Camera RAM steps ahead of displayed VRAM; verification must recover.
    f.word(0x02010000, 11);
    require(view.prepare(f.memory(), 569) == ViewStatus::Ready, "one-metatile VBlank lag not resolved");
    f.word(0x02010000, 14);
    require(view.prepare(f.memory(), 569) == ViewStatus::Unverified, "unverified camera did not fail closed");
    f.word(0x02010000, 10);
    // Nonzero subpixel scrolling and a ring wrap must retain the same map.
    f.iwram[0xe22] = 30; f.iwram[0xe23] = 28;
    for (int bg=1; bg<=3; ++bg) { f.reg(0x10+bg*4, 247); f.reg(0x12+bg*4, 229); }
    f.ring(20, 10, 30, 28);
    require(view.prepare(f.memory(), 569) == ViewStatus::Ready, "wrapped camera mapping failed");
    std::uint16_t tile = 0;
    require(view.tile(2, -8, 0, &tile) && tile == 0x1449, "negative margin floor or quadrant wrong");
    f.reg(0, 0x6e00);
    require(view.prepare(f.memory(), 569) == ViewStatus::Ready, "ordinary field UI windows rejected");
    f.reg(0, 0x0e80);
    require(view.prepare(f.memory(), 569) == ViewStatus::Unsupported, "forced-blank transition not suppressed");
    f.reg(0, 0x0e00);
    f.dword(0x03005dc8, 0x0203ffff);
    require(view.prepare(f.memory(), 569) == ViewStatus::Unsupported, "invalid map pointer accepted");
    f.dword(0x030022c4, 0x08000001);
    require(view.prepare(f.memory(), 569) == ViewStatus::NonField, "battle/menu retained stale world");
    require(!view.tile(1, 240, 0, &tile), "stale margin survived fallback");
    require(!f.memory().bytes(0x0203ffff, 2) && !f.memory().bytes(0x0800ffff, 2) &&
            !f.memory().bytes(0xfffffff0, 32), "region boundary reads not rejected");
    Fixture secondary;
    secondary.word(0x02000000 + (5*40+25)*2, 0xCE00); // secondary id 512, collision/elevation
    secondary.word(0x08003000, 0x2222);
    secondary.word(0x08003008, 0x4444);
    secondary.word(0x08005400, 0x2000); // SPLIT layers
    require(view.prepare(secondary.memory(), 569) == ViewStatus::Ready, "secondary tileset rejected");
    require(view.tile(1, 240, 0, &tile) && tile == 0x4444, "secondary top layer wrong");
    require(view.tile(2, 240, 0, &tile) && tile == 0, "split middle layer not transparent");
    require(view.tile(3, 240, 0, &tile) && tile == 0x2222, "secondary bottom layer wrong");
    secondary.word(0x08005400, 0x1000); // COVERED layers
    require(view.prepare(secondary.memory(), 569) == ViewStatus::Ready, "covered metatile rejected");
    require(view.tile(1, 240, 0, &tile) && tile == 0, "covered top layer not transparent");
    require(view.tile(2, 240, 0, &tile) && tile == 0x4444, "covered middle layer wrong");
    secondary.word(0x02000000 + (5*40+25)*2, 0x3FF);
    secondary.word(0x08000200, 7);
    require(view.prepare(secondary.memory(), 569) == ViewStatus::Ready, "authored border rejected");
    require(view.tile(1, 240, 0, &tile) && tile == 0x143C, "undefined cell did not use authored border");
    std::puts("Emerald field view: unseen tiles, camera lag/wrap, bounds, fallback, and read-only checks passed");
}
