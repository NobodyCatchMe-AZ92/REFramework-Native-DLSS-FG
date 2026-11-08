#pragma once

#include <windows.h>

#include <string>

#include <d3d12.h>
#include <wrl/client.h>

#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss.h>

#include <reframework/API.h>

// DLSS Frame Generation through NVIDIA Streamline, as a REFramework plugin.
//
// Only the game's side is hooked: the exe's CreateDXGIFactory imports and, through them, the game's swapchain, which is
// created through Streamline's proxy factory. REFramework's own hooks (on the native swapchain, which Streamline
// presents to for real and generated frames) are left alone; that is how REFramework works with games that ship DLSS-G.
namespace framegen {
// From DllMain (REFramework loads plugins very early): paths, settings and the import hooks. Must not wait on anything.
void early_init(HMODULE self);

// reframework_plugin_initialize: registers REFramework callbacks. Returns false if this REFramework can't be used.
bool initialize(const REFrameworkPluginInitializeParam* param);

// For the DLSS pass: Streamline's DLSS functions (nullptr if DLSS super resolution is unavailable or off), the settings,
// and the frame token of the frame being rendered (the one its Present will use; created if it doesn't exist yet).
struct Dlss {
    PFun_slSetConstants* set_constants{};
    PFun_slSetTagForFrame* set_tag_for_frame{};
    PFun_slEvaluateFeature* evaluate_feature{};
    PFun_slDLSSGetOptimalSettings* get_optimal_settings{};
    PFun_slDLSSSetOptions* set_options{};
    sl::ViewportHandle viewport{0};
    int mode{};   // setting dlss_mode: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int preset{}; // setting dlss_preset: 0 = default, else sl::DLSSPreset
    int debug{};  // setting dlss_debug
    bool bloom{}; // setting dlss_bloom
    bool lod_bias{};         // setting dlss_lod_bias
    float lod_bias_offset{}; // setting dlss_lod_bias_offset (mip levels, added to log2(render / output))
    int skip_draws{1};       // setting dlss_skip_draws: the engine's upscale draws replaced by DLSS
    float bloom_alpha{0.1f};  // setting dlss_bloom_alpha: the bloom stabilizer's weight of this frame
    float bloom_moving{};     // setting dlss_bloom_moving: its moving limit (render px), 0 = the profile's
    bool bloom_stabilize{};  // setting dlss_bloom_stabilize: a temporally stabilized scene for the engine's bloom
    float guard{};           // setting dlss_guard: upscale guard threshold (0 = off: DLSS replaces the engine's draws unchecked)
    int lod_bias_key{};      // setting dlss_lod_bias_key: virtual key toggling the LOD bias live (0 = none)
    int toggle_key{};        // setting dlss_toggle_key: virtual key switching DLSS off/on live (0 = none)
    bool hud_fix{};          // setting dlss_hud_fix: HUD-less frames for frame generation
    bool fg_enabled{};       // setting enabled: frame generation on (the HUD fix is only needed then)
    bool cut_reset{};        // setting dlss_cut_reset: reset DLSS / DLSS-G history on camera cuts
    int hud_fix_key{};       // setting hud_fix_key: virtual key switching the HUD fix off/on live (0 = none)
    bool sharpening_off{};   // setting dlss_sharpening_off (and no other mod doing it): the engine's sharpening off while DLSS is on
    int engine_aa{-1};       // setting dlss_engine_aa (experiments): -1 = the game's TAA set to none while DLSS is on, else this value
    int dither_test{0};        // setting dlss_dither_test (RE2 [EXPERIMENTAL] dither fix: 0 off, else on)
    bool no_upscale_fallback{true}; // setting dlss_no_upscale_fallback (diagnostics)
    bool examine_dlaa{};            // setting dlss_examine_dlaa: DLAA while examining items (profile examine_dlss_off, RE2)
    bool half_limit{true};      // setting dlss_half_limit (RE7: never below half the output)
    float rate_override{};      // setting dlss_rate_override (diagnostics: the render scale forced, 0 = the mode's)
    int guard_log{};                // setting dlss_guard_log: the upscale guard's log interval in frames (0 = 600)
    bool guard_live{true};          // setting dlss_guard_live: the profile's guard live rule (RE2 item box)
    bool native_path{true};         // setting dlss_native_path: DLSS on RE2's typewriter path from the scene color
    bool examine_stabilize{true};   // setting dlss_examine_stabilize: RE2 item examine: DLSS's output stabilized where still
    float examine_blend{0.1f};      // setting dlss_examine_blend: its weight of the new frame (1 = none)
    bool examine_no_jitter{true};   // setting dlss_examine_no_jitter: RE2 item examine without jitter (its outline shimmered)
    bool compute_input{true};       // setting dlss_compute_input: RE4: DLSS from its upscale pass's own input (depth of field in it)
    float bloom_transfer{1.0f};     // setting dlss_bloom_transfer: RE4: its bloom onto DLSS's image, strength (0 = off)
    bool own_bloom{true};           // setting dlss_own_bloom: RE8: bloom after DLSS instead of the game's before it
    float own_bloom_strength{1.0f}; // setting dlss_own_bloom_strength
    bool own_bloom_add{true};
    float own_bloom_threshold{-1.0f};
    float own_bloom_inner{-1.0f}; // setting dlss_own_bloom_threshold           // setting dlss_own_bloom_add
    bool own_motion_blur{true};     // setting dlss_own_motion_blur: RE7: motion blur after DLSS instead of the game's before it
    bool tape_bicubic{true};    // setting dlss_tape_scaler: videotape mode scaled with Catmull-Rom (else bilinear)
    bool tape_sharpening{true}; // setting dlss_tape_sharpening: the game's sharpening kept in videotape mode
    bool tape_off{};            // setting dlss_tape_off: DLSS off in videotape scenes (native, the game's own TAA)
    bool own_dof{true};         // setting dlss_own_dof: the plugin's depth of field (profile own_dof)
    float dof_scale{1.0f};      // setting dlss_dof_scale
    bool cas{true};             // setting dlss_cas: FidelityFX CAS sharpening on DLSS's output
    float cas_strength{0.5f};   // setting dlss_cas_strength (0..1)
    bool probe_dof{};           // setting probe_dof (diagnostics)
    float dof_max_scale{1.0f};  // setting dlss_dof_max_scale: the depth of field's largest blur against the game's
    float dof_temporal{0.15f};  // setting dlss_dof_temporal: the circle of confusion's weight of this frame (1: no smoothing over time)
};
const Dlss* dlss();
const std::string& game(); // REFramework's game name ("RE2", "RE4", ...)
sl::FrameToken* render_frame_token();
void constants_set(sl::FrameToken* token); // the DLSS pass set this frame's constants (Present then doesn't)
std::string describe_backbuffers(); // diagnostics: the game's swapchain buffers (addresses, size, format)

// This frame's inputs for frame generation, from the DLSS pass (copies that stay valid until Present): depth and motion
// vectors at render size, and the camera constants DLSS-SR used.
struct FrameInputs {
    ID3D12Resource* depth{};
    uint32_t depth_state{};
    ID3D12Resource* motion{};
    uint32_t motion_state{};
    uint32_t width{};
    uint32_t height{};
    sl::Constants constants{};
};
void set_frame_inputs(const FrameInputs& inputs);

// The HUD fix: the swapchain buffer the game is drawing this frame into, and this frame's HUD-less copy of it (tagged at
// Present as DLSS-G's hudless color).
Microsoft::WRL::ComPtr<ID3D12Resource> current_backbuffer();
void set_hudless(ID3D12Resource* hudless, uint32_t state);
} // namespace framegen
