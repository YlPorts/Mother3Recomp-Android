#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gbarecomp { struct ExtendedViewFrameInfo; }

namespace emerald {

constexpr int kMaxViewWidth = 569; // round(160 * 32 / 9)
constexpr int kMaxViewHeight = 854; // includes phone portrait and 9:32

// Borrowed, read-only guest regions. Reads never touch the emulated bus, its
// prefetch state, CPU cycles, or guest memory. Also used by capture tests.
struct ViewMemory {
    const std::uint8_t* ewram = nullptr;
    const std::uint8_t* iwram = nullptr;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_size = 0;
    const std::uint8_t* vram = nullptr;
    const std::uint8_t* io = nullptr;
    const std::uint8_t* oam = nullptr;
    const std::uint8_t* pal = nullptr;
    const std::uint8_t* bytes(std::uint32_t address, std::size_t size) const;
    std::uint16_t u16(std::uint32_t address) const;
    std::uint32_t u32(std::uint32_t address) const;
};

enum class ViewStatus { Native, NonField, Unsupported, Unverified, Ready };
const char* view_status_name(ViewStatus status);

class FieldView {
public:
    ViewStatus prepare(const ViewMemory& memory, int width, int height = 160);
    bool tile(int bg, int hardware_x, int screen_y, std::uint16_t* entry) const;
    ViewStatus status() const { return status_; }
    int compared() const { return compared_; }
    int matched() const { return matched_; }
private:
    static constexpr int kColumns = 73; // 569px plus a partial tile at either end
    static constexpr int kRows = 108;
    std::array<std::array<std::uint16_t, kColumns * kRows>, 3> tiles_{};
    ViewStatus status_ = ViewStatus::Native;
    int left_ = 0, top_ = 0, width_ = 240, height_ = 160, phase_x_ = 0, phase_y_ = 0;
    int compared_ = 0, matched_ = 0;
};

void install_extended_view(std::uint32_t left, std::uint32_t right);
void update_extended_view(const gbarecomp::ExtendedViewFrameInfo* frame);
void reset_extended_view();

} // namespace emerald
