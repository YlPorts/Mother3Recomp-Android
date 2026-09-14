#include "emerald_extended_view.h"
#include "emerald_object_view.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "mod_runtime.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"

#include <cstdio>
#include <cstring>

namespace emerald {
namespace {
constexpr const char* kPackage = "pokemon-emerald.enhancement.widescreen";
FieldView view;
ObjectView objects;
bool last_objects_ready = true;
bool enabled = false;
ViewStatus last_status = ViewStatus::Native;

int tile_provider(int bg, int x, int y, std::uint16_t* entry) {
    return view.tile(bg, x, y, entry) ? gba::kWsTilemapReplace : gba::kWsTilemapUnavailable;
}
const gba::WsMarginObjPixel* object_provider(int y, int* left, int* width) {
    return objects.row(y, left, width);
}

void activate() {
    const char* aspect = gba_mod_option_value(kPackage, "widescreen", "aspect");
    const bool adaptive = !aspect || std::strcmp(aspect, "fit") == 0;
    const int width = aspect && std::strcmp(aspect, "32:9") == 0 ? 569 :
                      aspect && std::strcmp(aspect, "21:9") == 0 ? 373 : 284;
    enabled = true;
    gba_mod_set_adaptive_view_enabled(adaptive);
    gba_mod_set_view_width(width);
}
} // namespace

void reset_extended_view() {
    enabled = false;
    view = FieldView{};
    // prepare clears every frame; removing the hook also invalidates a reset.
    last_objects_ready = true;
    last_status = ViewStatus::Native;
    if (gba::g_ws_tilemap_provider == tile_provider) {
        gba::g_ws_tilemap_provider = nullptr;
        gba::g_ws_authored_margin_layers = 0;
        gba::g_ws_obj_native_clip = 0;
        gba::g_ws_obj_margin_provider = nullptr;
        gba::g_ws_pillarbox = 0;
    }
    gba_mod_set_adaptive_view_enabled(0);
    gba_mod_set_view_width(0);
}

void install_extended_view(std::uint32_t, std::uint32_t) {
    if (!enabled) return;
    gba::g_ws_tilemap_provider = tile_provider;
    gba::g_ws_authored_margin_layers = 1;
    // Keep arbitrary parked OAM native. Verified overworld objects have a
    // separate layer, including sprites suppressed by the guest's X culler.
    gba::g_ws_obj_native_clip = 1;
    gba::g_ws_obj_margin_provider = object_provider;
    gba::g_ws_pillarbox = 1;
}

void update_extended_view(const gbarecomp::ExtendedViewFrameInfo* frame) {
    if (!enabled || !frame) return;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return;
    const ViewMemory memory{bus->ewram_ptr(), bus->iwram_ptr(), bus->rom_ptr(),
                           bus->rom_size(), bus->vram_ptr(), frame->io,
                           bus->oam_ptr(), bus->pal_ptr()};
    const auto status = view.prepare(memory, frame->view_width);
    const bool objects_ready = objects.prepare(memory, view, frame->view_width);
    if (status == ViewStatus::Ready && objects_ready != last_objects_ready) {
        std::fprintf(stderr, "[emerald:objects] %s frame=%llu objects=%d verified-parts=%d\n",
            objects_ready ? "verified" : "DEGRADED: OAM mismatch, native objects only",
            static_cast<unsigned long long>(frame->frame_count), objects.objects(), objects.verified_parts());
        last_objects_ready = objects_ready;
        if (!objects_ready) {
            const auto& d = objects.mismatch();
            std::fprintf(stderr,"[emerald:objects] object=%u expected=%04X,%04X,%04X hardware=%04X,%04X,%04X\n",
                d[0],d[1],d[2],d[3],d[4],d[5],d[6]);
        }
    }
    gba::g_ws_pillarbox = status == ViewStatus::Ready ? 0 : 1;
    if (status != last_status) {
        std::fprintf(stderr, "[emerald:view] %s frame=%llu width=%u tile-check=%d/%d%s\n",
                     view_status_name(status), static_cast<unsigned long long>(frame->frame_count),
                     frame->view_width, view.matched(), view.compared(),
                     status == ViewStatus::Unverified ? " DEGRADED: native fallback" : "");
        last_status = status;
    }
}
} // namespace emerald

GBA_MOD_CONSTRUCTOR(emerald_register_adaptive_view_plugin) {
    gba_mod_register_reset_callback(emerald::reset_extended_view);
    gba_mod_register_activation_plugin("pokemon-emerald.widescreen", emerald::activate);
}
