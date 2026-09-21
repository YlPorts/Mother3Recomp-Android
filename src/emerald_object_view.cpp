#include "emerald_object_view.h"
#include <algorithm>
#include <cstdlib>

namespace emerald {
namespace {
constexpr std::uint32_t kObjects = 0x02037350;
constexpr std::uint32_t kSprites = 0x02020630;
constexpr std::uint32_t kOffsetX = 0x02021BBC;
constexpr std::uint32_t kOffsetY = 0x02021BBE;
unsigned u16(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
int s16(const std::uint8_t* p) { return static_cast<std::int16_t>(u16(p)); }
int s8(std::uint8_t b) { return static_cast<std::int8_t>(b); }
int unwrap(int raw, int reference, int modulus) {
    const int delta = ((raw - reference + modulus / 2) % modulus + modulus) % modulus;
    return reference + delta - modulus / 2;
}
constexpr int widths[3][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32}};
constexpr int heights[3][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64}};
struct Part { int x, y; unsigned a0, a1, a2; };
}

bool ObjectView::prepare(const ViewMemory& m, const FieldView& field, int width, int height) {
    ready_ = false;
    objects_ = verified_parts_ = 0;
    mismatch_.fill(0);
    width_ = std::clamp(width, 240, kMaxViewWidth);
    left_ = -(width_ - 240) / 2;
    height_ = std::clamp(height, 160, kMaxViewHeight);
    top_ = -(height_ - 160) / 2;
    if (field.status() != ViewStatus::Ready || !m.oam || !m.pal || !m.vram || !m.io)
        return false;
    pixels_.assign(width_ * height_, {});
    const int offset_x = static_cast<std::int16_t>(m.u16(kOffsetX));
    const int offset_y = static_cast<std::int16_t>(m.u16(kOffsetY));
    const bool mapping_1d = (u16(m.io) & 0x40) != 0;
    // Decode every visible object as well, so the hardware OAM continuously
    // checks our Sprite/Subsprite layout before we use it outside the screen.
    for (int id = 0; id < 16; ++id) {
        const auto* o = m.bytes(kObjects + id * 36, 36);
        if (!o || !(o[0] & 1) || (o[1] & 0x20)) continue; // inactive/script-hidden
        if (o[4] >= 64) return false;
        const auto address = kSprites + o[4] * 68;
        const auto* s = m.bytes(address, 68);
        if (!s || !(s[62] & 1)) return false;
        if ((s[62] & 4) && !(o[1] & 0x40)) continue; // other intentional hiding
        const unsigned a0 = u16(s), a1 = u16(s+2), a2 = u16(s+4);
        // Affine, bitmap, mosaic, and OBJ-window field effects are not NPCs.
        if ((a0 & 0x3F00) || (a0 >> 14) >= 3) continue;
        int x = s16(s+32) + s16(s+36) + s8(s[40]);
        int y = s16(s+34) + s16(s+38) + s8(s[41]);
        if (s[62] & 2) { x += offset_x; y += offset_y; }
        // Sprite simulation can already be one step ahead of the OAM that
        // VBlank published. Visible sprites retain those published coordinates
        // in Sprite.oam; use RAM positions only to unwrap their 9/8-bit fields.
        if (!(o[1] & 0x40)) {
            x = unwrap(a1 & 511, x, 512);
            y = unwrap(a0 & 255, y, 256);
        }
        const bool hflip = (a1 & 0x1000) != 0, vflip = (a1 & 0x2000) != 0;
        std::array<Part, 16> parts{};
        int count = 1;
        parts[0] = {x, y, a0, a1, a2};
        const auto tables = m.u32(address + 24);
        const int mode = s[66] >> 6;
        if (tables && mode) {
            const auto* table = m.bytes(tables + (s[66] & 63) * 8, 8);
            if (!table || table[0] > parts.size()) return false;
            count = table[0];
            const auto* data = m.bytes(m.u32(tables + (s[66] & 63) * 8 + 4), count * 4);
            if (!data && count) return false;
            for (int n = 0; n < count; ++n) {
                const auto* p = data + n * 4;
                const unsigned bits = u16(p+2), shape = bits & 3, size = (bits >> 2) & 3;
                if (shape >= 3) return false;
                const int dx = hflip ? -s8(p[0]) - widths[shape][size] : s8(p[0]);
                const int dy = vflip ? -s8(p[1]) - heights[shape][size] : s8(p[1]);
                parts[n] = {x - s8(s[40]) + dx, y - s8(s[41]) + dy,
                    (a0 & 0x3FFF) | (shape << 14), (a1 & 0x3FFF) | (size << 14),
                    (a2 & 0xFC00) | (((a2 & 1023) + ((bits >> 4) & 1023)) & 1023)};
                if (mode != 2) parts[n].a2 = (parts[n].a2 & ~0xC00u) | ((bits >> 14) << 10);
            }
        }
        ++objects_;
        for (int n = 0; n < count; ++n) {
            auto& p = parts[n];
            p.a0 = (p.a0 & ~255u) | (p.y & 255);
            p.a1 = (p.a1 & ~511u) | (p.x & 511);
            int order = 127;
            bool found = false;
            for (int idx = 0; idx < 128; ++idx) {
                const auto* hw = m.oam + idx * 8;
                const unsigned hw0 = u16(hw), hw1 = u16(hw+2), hw2 = u16(hw+4);
                const int hx = unwrap(hw1 & 511, p.x, 512), hy = unwrap(hw0 & 255, p.y, 256);
                // OAM publication can trail Sprite.oam too (the game builds
                // next frame's list during visible time). Bind by allocated
                // tile/palette, shape and size, allowing at most one 8px step.
                // Published coordinates and flips then remain authoritative.
                if ((hw0 & 0xFF00) == (p.a0 & 0xFF00) &&
                    (hw1 & 0xC000) == (p.a1 & 0xC000) && hw2 == p.a2 &&
                    std::abs(hx - p.x) <= 8 && std::abs(hy - p.y) <= 8) {
                    if (found) return false; // ambiguous identity must not draw
                    order = idx; found = true;
                }
            }
            if (!(o[1] & 0x40)) { // Offscreen sprites deliberately have no OAM.
                if (!found) {
                    mismatch_ = {unsigned(id),p.a0,p.a1,p.a2,0,0,0};
                    for (int idx = 0; idx < 128; ++idx) {
                        const auto* hw = m.oam + idx * 8;
                        if (u16(hw+4) == p.a2) {
                            mismatch_[4]=u16(hw); mismatch_[5]=u16(hw+2); mismatch_[6]=u16(hw+4); break;
                        }
                    }
                    return false;
                }
                ++verified_parts_;
            }
            if (found) {
                const auto* hw = m.oam + order * 8;
                p.a0 = u16(hw); p.a1 = u16(hw+2); p.a2 = u16(hw+4);
                p.x = unwrap(p.a1 & 511, p.x, 512);
                p.y = unwrap(p.a0 & 255, p.y, 256);
            }
            const int shape = p.a0 >> 14, size = p.a1 >> 14;
            const int w = widths[shape][size], h = heights[shape][size];
            const bool part_hflip = (p.a1 & 0x1000) != 0, part_vflip = (p.a1 & 0x2000) != 0;
            const int priority = (p.a2 >> 10) & 3, palette = p.a2 >> 12;
            for (int py = std::max(0, top_ - p.y); py < h && p.y + py < top_ + height_; ++py) {
                const int ty = part_vflip ? h - 1 - py : py;
                for (int px = 0; px < w; ++px) {
                    const int hx = p.x + px, out_x = hx - left_;
                    if (out_x < 0 || out_x >= width_ ||
                        (hx >= 0 && hx < 240 && p.y + py >= 0 && p.y + py < 160)) continue;
                    const int tx = part_hflip ? w - 1 - px : px;
                    const unsigned tile = (p.a2 & 1023) + (ty / 8) * (mapping_1d ? w / 8 : 32) + tx / 8;
                    const unsigned offset = 0x10000 + tile * 32 + (ty & 7) * 4 + (tx & 7) / 2;
                    if (offset >= 0x18000) return false;
                    const unsigned index = (m.vram[offset] >> ((tx & 1) * 4)) & 15;
                    if (!index) continue;
                    auto& dest = pixels_[(p.y + py - top_) * width_ + out_x];
                    if (!(dest.color & 0x8000) && dest.priority * 256 + dest.order <= priority * 256 + order) continue;
                    dest = {static_cast<std::uint16_t>(u16(m.pal + 0x200 + palette * 32 + index * 2) & 0x7FFF),
                            static_cast<std::uint8_t>(priority), static_cast<std::uint8_t>(order)};
                }
            }
        }
    }
    ready_ = true;
    return true;
}

const gba::WsMarginObjPixel* ObjectView::row(int y, int* left, int* width) const {
    if (!ready_ || y < top_ || y >= top_ + height_ || !left || !width) return nullptr;
    *left = left_; *width = width_;
    return pixels_.data() + (y - top_) * width_;
}
} // namespace emerald
