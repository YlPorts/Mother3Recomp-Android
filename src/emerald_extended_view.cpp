#include "emerald_extended_view.h"

#include <algorithm>

namespace emerald {
namespace {
// USA Emerald, SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7.
// Addresses: variants/emerald/symbols/BPEE_symbols.toml. Structure/asset
// layouts: pinned pret/pokeemerald include/global.fieldmap.h, field_camera.c.
// The reference is used as a data-format description, never executed here.
constexpr std::uint32_t kMain = 0x030022C0;
constexpr std::uint32_t kOverworld = 0x08085E5C;
constexpr std::uint32_t kMapHeader = 0x02037318;
constexpr std::uint32_t kBackupMap = 0x03005DC0;
constexpr std::uint32_t kSaveBlock1Ptr = 0x03005D8C;
constexpr std::uint32_t kCameraOffset = 0x03000E20;

int floor8(int value) { return value >= 0 ? value / 8 : -((-value + 7) / 8); }
int floor2(int value) { return value >= 0 ? value / 2 : -((-value + 1) / 2); }
int wrap(int value, int modulus) { return (value % modulus + modulus) % modulus; }
int signed_ring_delta(int value) { return wrap(value + 128, 256) - 128; }
unsigned read16(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }

struct Map {
    const ViewMemory& m;
    std::uint32_t data = 0, border = 0;
    std::uint32_t metatiles[2]{}, attributes[2]{};
    int width = 0, height = 0;

    bool load() {
        const auto layout = m.u32(kMapHeader);
        if (!m.bytes(layout, 24)) return false;
        width = static_cast<int>(m.u32(kBackupMap));
        height = static_cast<int>(m.u32(kBackupMap + 4));
        data = m.u32(kBackupMap + 8);
        border = m.u32(layout + 8);
        if (width < 1 || height < 1 || width > 10240 || height > 10240 ||
            width * height > 10240 || !m.bytes(data, width * height * 2) ||
            !m.bytes(border, 8)) return false;
        for (int i = 0; i < 2; ++i) {
            const auto tileset = m.u32(layout + 16 + i * 4);
            if (!m.bytes(tileset, 24)) return false;
            metatiles[i] = m.u32(tileset + 12);
            attributes[i] = m.u32(tileset + 16);
            if (!m.bytes(metatiles[i], 16) || !m.bytes(attributes[i], 2))
                return false;
        }
        return true;
    }

    bool entries(int tile_x, int tile_y, std::uint16_t out[3]) const {
        const int x = floor2(tile_x), y = floor2(tile_y);
        unsigned block = 0x3ff;
        if (x >= 0 && y >= 0 && x < width && y < height)
            block = m.u16(data + 2 * (x + y * width));
        // The live padded map includes connections and map edits. Undefined
        // or out-of-bounds cells use the map's authored 2x2 border pattern.
        if (block == 0x3ff)
            block = m.u16(border + 2 * (wrap(x + 1, 2) + 2 * wrap(y + 1, 2)));
        const unsigned id = block & 0x3ff;
        const unsigned set = id / 512, index = id % 512;
        const auto tiles = metatiles[set] + index * 16;
        const auto attr = attributes[set] + index * 2;
        if (!m.bytes(tiles, 16) || !m.bytes(attr, 2)) return false;
        const unsigned layer = m.u16(attr) >> 12;
        const int quadrant = wrap(tile_x, 2) + 2 * wrap(tile_y, 2);
        const auto bottom = m.u16(tiles + quadrant * 2);
        const auto top = m.u16(tiles + 8 + quadrant * 2);
        switch (layer) {
        case 0: out[0] = top; out[1] = bottom; out[2] = 0x3014; break;
        case 1: out[0] = 0; out[1] = top; out[2] = bottom; break;
        case 2: out[0] = top; out[1] = 0; out[2] = bottom; break;
        default: return false;
        }
        return true;
    }
};
} // namespace

const std::uint8_t* ViewMemory::bytes(std::uint32_t address, std::size_t size) const {
    const std::uint8_t* base = nullptr;
    std::size_t offset = 0, capacity = 0;
    if (address >= 0x02000000 && address < 0x02040000) {
        base = ewram; offset = address - 0x02000000; capacity = 0x40000;
    } else if (address >= 0x03000000 && address < 0x03008000) {
        base = iwram; offset = address - 0x03000000; capacity = 0x8000;
    } else if (address >= 0x08000000 && address < 0x0A000000) {
        base = rom; offset = address - 0x08000000; capacity = rom_size;
    }
    return base && offset <= capacity && size <= capacity - offset ? base + offset : nullptr;
}
std::uint16_t ViewMemory::u16(std::uint32_t address) const {
    const auto* p = bytes(address, 2);
    return p ? static_cast<std::uint16_t>(read16(p)) : 0;
}
std::uint32_t ViewMemory::u32(std::uint32_t address) const {
    const auto* p = bytes(address, 4);
    return p ? read16(p) | (read16(p + 2) << 16) : 0;
}

const char* view_status_name(ViewStatus status) {
    switch (status) {
    case ViewStatus::Native: return "native";
    case ViewStatus::NonField: return "non-field";
    case ViewStatus::Unsupported: return "unsupported-field";
    case ViewStatus::Unverified: return "unverified-camera";
    case ViewStatus::Ready: return "verified-field";
    }
    return "unknown";
}

ViewStatus FieldView::prepare(const ViewMemory& m, int width) {
    status_ = ViewStatus::Native;
    compared_ = matched_ = 0;
    width_ = std::clamp(width, 240, kMaxViewWidth);
    left_ = (width_ - 240) / 2;
    if (width_ == 240) return status_;
    status_ = ViewStatus::NonField;
    if ((m.u32(kMain + 4) & ~1u) != kOverworld) return status_;
    status_ = ViewStatus::Unsupported;
    if (!m.io || !m.vram || !m.iwram) return status_;
    const unsigned dispcnt = read16(m.io);
    // Mode 0, all three field backgrounds, no forced blank. Emerald keeps
    // rectangular hardware windows enabled during ordinary field rendering;
    // these remain native UI masks. BG0 never extends into the margins.
    if ((dispcnt & 0x87) || (dispcnt & 0xE00) != 0xE00) return status_;
    const int hofs = read16(m.io + 0x14) & 255;
    const int vofs = read16(m.io + 0x16) & 255;
    unsigned screen_base[3]{};
    for (int bg = 1; bg <= 3; ++bg) {
        const auto cnt = read16(m.io + 8 + bg * 2);
        if ((cnt & 0xC080) || ((cnt & 0x40) && (m.io[0x4C] != 0)) ||
            int(read16(m.io + 0x10 + bg * 4) & 255) != hofs ||
            int(read16(m.io + 0x12 + bg * 4) & 255) != vofs) return status_;
        screen_base[bg - 1] = ((cnt >> 8) & 31) * 0x800;
    }
    Map map{m};
    const auto save = m.u32(kSaveBlock1Ptr);
    if (!map.load() || !m.bytes(save, 4)) return status_;
    const auto* camera = m.bytes(kCameraOffset, 4);
    if (!camera || camera[2] >= 32 || camera[3] >= 32) return status_;
    const int nominal_x = static_cast<std::int16_t>(m.u16(save)) * 16 +
        signed_ring_delta(hofs - camera[2] * 8);
    const int nominal_y = static_cast<std::int16_t>(m.u16(save + 2)) * 16 +
        signed_ring_delta(vofs - camera[3] * 8);
    const int origin_tile_x = floor8(nominal_x), origin_tile_y = floor8(nominal_y);
    phase_x_ = hofs & 7;
    phase_y_ = vofs & 7;

    // Camera RAM may have advanced one metatile before VBlank publishes the
    // ring. Resolve that bounded ambiguity against the displayed VRAM. Every
    // sampled native tile/layer must match, on every frame; never accumulate
    // camera deltas or retain a prior-map cache across warps/savestate loads.
    const auto score = [&](int dx, int dy) {
        int matches = 0, total = 0;
        for (int y = 0; y < 20; y += 3) for (int x = 0; x < 30; x += 3) {
            std::uint16_t expected[3];
            if (!map.entries(origin_tile_x + x + dx, origin_tile_y + y + dy, expected))
                return -1;
            const int ring = wrap((hofs >> 3) + x, 32) + 32 * wrap((vofs >> 3) + y, 32);
            for (int bg = 0; bg < 3; ++bg) {
                ++total;
                matches += expected[bg] == read16(m.vram + screen_base[bg] + ring * 2);
            }
        }
        compared_ = total;
        return matches;
    };
    status_ = ViewStatus::Unverified;
    int best_x = 0, best_y = 0;
    matched_ = score(0, 0);
    for (int dy : {0, -2, 2}) for (int dx : {0, -2, 2}) {
        if (matched_ == compared_ && compared_ > 0) break;
        const int matches = score(dx, dy);
        if (matches > matched_) { matched_ = matches; best_x = dx; best_y = dy; }
    }
    if (compared_ == 0 || matched_ != compared_) return status_;
    const int first_x = floor8(-left_ + phase_x_);
    const int last_x = floor8(width_ - left_ - 1 + phase_x_);
    if (last_x - first_x >= kColumns) return status_;
    for (int y = 0; y < kRows; ++y) for (int x = first_x; x <= last_x; ++x) {
        std::uint16_t entries[3];
        if (!map.entries(origin_tile_x + best_x + x, origin_tile_y + best_y + y, entries))
            return status_;
        for (int bg = 0; bg < 3; ++bg)
            tiles_[bg][y * kColumns + x - first_x] = entries[bg];
    }
    status_ = ViewStatus::Ready;
    return status_;
}

bool FieldView::tile(int bg, int hardware_x, int screen_y, std::uint16_t* entry) const {
    if (status_ != ViewStatus::Ready || bg < 1 || bg > 3 || !entry ||
        hardware_x < -left_ || hardware_x >= width_ - left_ ||
        screen_y < 0 || screen_y >= 160) return false;
    const int x = floor8(hardware_x + phase_x_) - floor8(-left_ + phase_x_);
    const int y = (screen_y + phase_y_) / 8;
    *entry = tiles_[bg - 1][y * kColumns + x];
    return true;
}
} // namespace emerald
