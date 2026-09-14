#pragma once
#include "emerald_extended_view.h"
#include "gba_ppu.h"

namespace emerald {
// Decodes only live overworld object sprites. No guest calls, allocation,
// movement simulation, or persistent state across frames/save loads.
class ObjectView {
public:
    bool prepare(const ViewMemory& memory, const FieldView& field, int width);
    const gba::WsMarginObjPixel* row(int y, int* left, int* width) const;
    int objects() const { return objects_; }
    int verified_parts() const { return verified_parts_; }
    const std::array<unsigned, 7>& mismatch() const { return mismatch_; }
private:
    std::array<gba::WsMarginObjPixel, kMaxViewWidth * 160> pixels_{};
    int left_ = 0, width_ = 0, objects_ = 0, verified_parts_ = 0;
    bool ready_ = false;
    std::array<unsigned, 7> mismatch_{}; // object, expected attrs, same-tile HW attrs
};
} // namespace emerald
