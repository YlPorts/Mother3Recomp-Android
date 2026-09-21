# Emerald overworld widescreen experiment

This opt-in presentation plugin reveals additional map scenery at the original
pixel scale. In the launcher, open **Mods**, enable **Overworld Widescreen
(Experimental)**, choose its **Aspect ratio**, and apply the selection before
playing. The packaged feature defaults to disabled.

| Choice | Logical surface | Behavior |
| --- | --- | --- |
| Fit to window | 240–569 × 160, or 240 × 160–854 | Follows landscape and portrait window aspects |
| 16:9 | 284 × 160 | Fixed width, aspect-correct letterboxing when needed |
| 21:9 | 373 × 160 | Fixed width |
| 32:9 | 569 × 160 | Fixed width; 164 pixels left and 165 right |

The fractional ideal dimensions are rounded to the nearest logical pixel. Narrow
windows reveal scenery above/below the original view: 9:16 uses 240 × 427 and
9:20 uses 240 × 533. Portrait expansion requires v0.0.6 and mod 0.2.0;
the older v0.0.5 executable supports horizontal expansion only.
In headless tests, Fit uses a
284 × 160 initial surface because there is no host window to measure.

## Deliberate limits

- Scenery and **live overworld NPCs** extend into the margins, including NPC
  sprites hidden by the game's screen-edge culler. The guest's farther object
  spawn/despawn range is unchanged: this does not instantiate unloaded map
  events or simulate distant NPC movement. Field effects, reflections and
  non-object sprites still retain native visibility.
- Overworld BG0 windows anchor to the corresponding viewport edges: Start
  menu top-right, location banner top-left, dialogue bottom-center. Their
  original text, borders and cursors are sampled from live VRAM. Battles and
  full-screen interfaces retain the centered 240 × 160 image with black margins.
- Animated doors retain expanded scenery. Full map reloads still require
  published map data before the expanded scenery resumes, during the black fade.
- Tested scenes are Littleroot Town, Birch's lab, their door transitions, the
  initial wild battle and the nickname screen. Weather, cycling, surfing,
  caves, special maps and the rest of the game are not qualified by this test.

## Implementation

`src/emerald_extended_view.cpp` reads immutable views of the live padded map
buffer, camera state and ROM tileset data for the hash-gated USA cartridge. It
decodes primary/secondary metatiles, layer assignments and authored border
tiles. The live padded map supplies map edits and already-loaded connections.
The plugin feeds tile entries to the existing GBA expanded PPU, which renders
tile graphics, palettes, priority and blending from the guest's current state.

Before publishing a frame, the renderer compares 210 native tile/layer probes
against displayed VRAM. All probes must match, including exact animated-door
overrides validated against the USA ROM's door graphics table and palettes.
A bounded search resolves the
one-metatile difference between camera RAM and the published scrolling ring.
Failure removes the margins and emits an `unverified-camera ... DEGRADED`
transition in the log. No previous-map cache survives a warp or savestate load.
This verifies sampled tile mapping, not every offscreen effect in the game.

The guest executes normally. This plugin does not dispatch additional guest
functions, edit generated code, or write guest RAM, VRAM, palettes, OAM, saves
or ROM. Its state is host presentation state. The original 240 × 160 area
continues to use the normal PPU samples, except for explicitly relocated UI.

Portrait rows are host presentation only: the engine samples authored map and
live object providers once at frame start without advancing guest scanlines,
HBlank DMA or affine state. Save states retain their native framebuffer payload.
The shared capability defaults to a maximum height of 160 unless a game opts in.

`src/emerald_object_view.cpp` decodes live ObjectEvent/Sprite/Subsprite data
into a host-only margin layer. Each visible part must bind unambiguously to
hardware OAM by its allocated tiles, palette, shape, size and priority, within
one bounded 8-pixel publication step. Published OAM supplies positions and
flips; actual VRAM and PAL supply animation graphics and colors. This handles
Sprite RAM advancing ahead of the currently displayed OAM. Live objects
marked `offScreen` can still render from their software sprite; explicit
script invisibility and inactive objects remain hidden. Mismatches log
`DEGRADED` and suppress the object layer. Every frame starts fresh, including
save loads and warps. The engine consumes the layer only outside native X
0..239 and preserves BG priority and color effects. Authored scenery and NPC
margins bypass native window layer masks: Emerald's ordinary WINOUT disables
OBJ outside the original screen. Native window behavior remains unchanged.

The engine changes are generic: committed plugin option lookup, a resettable
fixed/initial view-width request, a 576-pixel framebuffer capacity, a read-only
OBJ margin layer, and the
same frame-policy callback in TCP and normal execution. Game capability limits
still apply. Disabling Emerald's feature overrides stale CLI/environment view
requests. The v0.0.5 release integrates these changes into `main` in both this
repository and the engine. Its engine and launcher gitlinks pin published
dependency revisions.

## Reproduction and validation

Build with the pinned engine checkout; no ROM regeneration is
needed for this presentation change:

```powershell
cmake --build build-rel-004 --target EmeraldRecomp emerald_extended_view_test emerald_view_capture_check mod_runtime_tests ppu_smoke_tests -j 6
ctest --test-dir build-rel-004 -R '^emerald_extended_view_test$' --output-on-failure
ctest --test-dir build-rel-004/gbarecomp_build -R '^(mod_runtime_tests|ppu_smoke_tests)$' --output-on-failure
```

Use the native MinGW CMake/Ninja executables on Windows. The game test covers
camera lag and ring wrap, negative coordinates, all three metatile layer
assignments, secondary tilesets, undefined cells/borders, invalid pointers,
scene fallback and the absence of guest writes.

`tools/widescreen_smoke.py` accepts `--exe`, `--bios`, `--rom`, `--state`,
`--output`, optional `--toolchain`, `--aspect`, `--frames` and `--route`
(`walk`, `left`, `right`, `left-right`, `doors-menu`). It creates
isolated native/wide copies, drives identical input, compares the central
image every eight frames, and compares RAM/VRAM/PAL/OAM at checkpoints. It
The `doors-menu` route uses the Sept 20 doorway F1 fixture, checks visible door
margins and Start-menu edge placement, and excludes published UI rectangles
from the native-center comparison while still comparing guest memory exactly.
It also checks that a stale `GBARECOMP_VIEW_WIDTH=569` cannot enable the disabled
feature. All game runs use `GBARECOMP_STRICT_STATIC=1`.

`tools/widescreen_resize_smoke.py` takes the same path arguments and tests the
experiment's own hidden Windows SDL window. It verifies adaptive 16:9, 21:9,
32:9, portrait and odd widths, plus fixed 21:9 in a 16:9 window. Use
`--portrait-only` to run only 4:5, 9:16 and 9:20 window cases.

`emerald_view_capture_check <directory> <ROM> [width]` inspects read-only TCP
captures (`ewram.bin`, `iwram.bin`, `vram.bin`, `io.bin`, `pal.bin`, `oam.bin`).
It verifies native tile mapping, checks the native center against the normal
PPU and writes an RGB preview. Captures, ROMs and saves remain local artifacts.

Validation on 2026-09-13: all three CTest targets passed. Walking runs at
16:9/21:9/32:9 and a headless Fit run had zero sampled central pixel differences
and identical guest memory checkpoints. The 720-frame 32:9 route entered and
left the lab, exercised native fallback, and recovered expanded scenery.
All runs reported zero dispatch misses, interpreted instructions and healed
native functions. All six real-window resize cases passed. This checks the
enhancement against the existing native renderer; no new mGBA differential
qualification of the base emulator is claimed.

NPC regression (2026-09-13, `beads-5ae.3.2`): the user's F1 save on Route 101
has local object 6 at native X=-16. The previous clip removed all 167 opaque
margin pixels. The new layer restores those pixels, verifies both live object
parts against OAM, and leaves the native center byte-identical. A 720-frame
left/right run at 32:9 and 180-frame leftward runs at 16:9 and 21:9 passed with
no object-layer degradation, zero sampled center differences, equal guest
memory and fully-static coverage. All six real-window resize cases also
passed from F1. Unit tests cover the software offscreen flag, explicit script
hiding, object removal, mismatched OAM, publication lag and BG depth. The
original F1 file is preserved; test states and captures live under the ignored
`build-npc-debug/` directory.

The first NPC validation checked decoded pixels but missed that WINOUT removed
every one in the final compositor. The follow-up adds the actual window-mask
regression and counts final visible pixels separately. Both the F1 and live
reported-state captures now have **167 decoded and 167 displayed** NPC margin
pixels. A new 720-frame walk preserves all native center samples and guest
memory, and the updated real window was captured with the NPC visibly present
while walking left. `emerald_view_capture_check <capture> <ROM> 569
--require-visible-objects` makes zero final NPC pixels a test failure for these
fixtures. Portrait expansion is tracked separately and is not in v0.0.5.

Development validation (2026-09-20): the new doorway fixture completed a
550-frame entry/exit/Start-menu route with 69 native-center comparisons outside
relocated UI, 25 identical memory checkpoints, 12 visible-door-margin checks,
10 exact Start-menu pixel comparisons, and fully static execution. The old NPC
fixture retained 167 decoded/displayed margin pixels; its 720-frame route had
90 unchanged native-center samples and 10 equal memory checkpoints. All eight
actual-window resize cases passed both from the doorway and with Start open.
Portrait door captures preserved the native center while filling all 64,080
pixels above/below a 240 × 427 view. Battle captures at 569 × 160 and 240 × 427
preserved the native image with zero nonblack margin pixels. Engine tests cover
vertical opt-in, odd-height splitting, bounds, scanout timing, snapshot crop,
source window/effect masks for relocated UI, and native-path isolation.
The SDL Fit presenter also rounds to physical pixels rather than requiring an
exact reduced logical ratio: a 540 × 960 window now displays the full portrait
world without the former 30-pixel side borders. This was verified from the
visible Direct3D11 preview, in addition to logical framebuffer captures. Three
portrait sizes were also tested with a live battle and had black margins around
the nonempty native battle image. The original F1 file remained unchanged.

Release artifact validation (2026-09-14): the extracted Windows ZIP passed a
720-frame 32:9 left/right run using its bundled toolchain, with 90 unchanged
native center comparisons, 10 matching guest memory checkpoints, and fully
static coverage. All six real-window resize cases passed against that same
shipped executable. The production mod installer accepted the separate
`.gbamod`, kept it disabled by default, and committed the enabled 32:9 option.
Both archives passed CRC and content checks; neither includes ROMs, BIOS,
saves, or local settings. The release includes `SHA256SUMS.txt` for both assets.
