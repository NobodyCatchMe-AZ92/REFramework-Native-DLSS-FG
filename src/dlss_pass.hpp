#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

// DLSS super resolution inside the engine's frame, where the engine upscales the scene itself (after PrePostEffect,
// before PostEffect): bloom and the other post effects then run at the output resolution on the upscaled image, and
// the HUD never goes through DLSS.
//
//  - The engine renders the scene at its own render scale (RenderConfig.ImageQualityRate), set to DLSS's ratio.
//  - The camera is jittered (Halton) in the scene's matrices after the engine's Scene update.
//  - The engine records its own commands while the layers draw and turns them into D3D12 later, so the pass is placed
//    with a marker: an engine Clear command with a color nothing else uses, queued right after PrePostEffect drew. When
//    the engine turns that into ClearRenderTargetView on one of its command lists, DLSS is recorded there instead
//    (HDRTarget + depth + velocity -> PostMainTarget), and the list's state put back (cl_state).
// Anything missing: the pass is skipped and the engine's own upscale stays.
namespace dlss_pass {
void on_device(ID3D12Device* device);                            // once the game's device is known (DLSS supported)
void on_begin_rendering();                                       // game thread: applies the render scale
bool examine_dlaa(); // RE2 item examine with dlss_examine_dlaa: DLSS's mode is DLAA meanwhile
bool half_limit_now(); // RE7's title: Ultra Performance runs as Performance meanwhile
void before_prepare_output(void* layer, void* render_context); // render thread: the missing-upscale fallback's marker (RE2)
void on_scene_updated(void* scene_layer);                        // after the engine's Scene update: jitter
void on_pre_post_effect_draw(void* layer, void* render_context); // render thread, after the layer drew: the marker
void before_pre_post_effect_draw(void* layer); // render thread, before it: diagnostics (dlss_debug=10, a trace of the whole post stage)
std::string status();                                            // for the status line (resets the counters)

// The factor for the scene's texture LOD scale (via.render.layer.Scene.get_LODBias): 2^(log2(render / output) + offset)
// for the main scene while DLSS runs (offset: setting dlss_lod_bias_offset, -1 as DLSS recommends), else 1.
float lod_scale(void* scene);

// For the status file: "key=value" lines (DLSS running, render and output size, render scale, LOD scale).
std::string status_values();

// For other passes (hud_split): the scene's full resolution target (PostMainTarget) and its render target view, and an
// engine Clear marker with `tag` as its 4th value (2000 + kind for HUD markers), handled when it reaches a command list.
void* scene_target_rtv(void* scene);
ID3D12Resource* scene_target(void* scene);
bool queue_marker(void* render_context, void* rtv, float tag);
} // namespace dlss_pass
