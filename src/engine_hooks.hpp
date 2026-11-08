#pragma once

#include <cstdint>

// The plugin's own hooks into the RE Engine renderer, installed through the game's type database (official REFramework
// plugin API) and chained with whatever else hooks the same functions (REFramework hooks several of them too):
//  - via.SceneView.get_Size (native): the render resolution, for DLSS.
//  - via.render.layer.Scene update, Overlay draw, PrepareOutput draw (the functions their vtables point to).
namespace engine_hooks {
// After REFramework has initialized (type database ready). Each hook reports its own success; returns true if all worked.
bool install();

// Calls per hook since the last call (for the status line), and resets them.
struct Counts {
    uint32_t view_get_size{};
    uint32_t scene_update{};
    uint32_t overlay_draw{};
    uint32_t prepare_output_draw{};
    uint32_t lod_bias{}; // via.render.layer.Scene.get_LODBias (engine and our status line)
    uint32_t sharpness{}; // via.render.ToneMapping.get_Sharpness (both engine variants)
};
Counts take_counts();

// While on (the DLSS pass sets it per frame): the engine's tone mapping sharpening reads as 0, its forced sharpening
// (fixed values for some TAA modes) included. DLSS's output is sharp already; the engine's sharpening on top of it
// oversharpens (the same thing as the RE2 "disable sharpening" script and RE4.Sharpness do).
void set_zero_sharpness(bool zero);
bool sharpness_hooked();
void set_no_temporal_aa(bool off); // RE2: ToneMapping.getTemporalAA reports none while true (profile temporal_aa_off)
} // namespace engine_hooks
