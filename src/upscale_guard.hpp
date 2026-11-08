#pragma once

#include <d3d12.h>

#include <cstdint>

// Shows DLSS's output only where the engine really upscaled the post color. The engine's own upscale draw runs first
// (into the scene target); then, on a grid of samples, its result E is compared with a plain bilinear upscale S of the
// post color: k = sum(e*s) / sum(s*s), r = sum((e - k*s)^2) / sum(e^2) (luminance). A match (r below the threshold)
// gives DLSS * k (k carries a fade the engine applied), anything else the engine's own result: during the splash
// screens and menu transitions the engine draws black or other content there, while the post color is stale.
// Compute shaders recorded into the game's command list (the caller saves and restores the list's state).
namespace upscale_guard {
bool init(ID3D12Device* device);

// States on entry and exit: engine (the scene target, with the engine's result), post and dlss readable by compute
// shaders, out in UNORDERED_ACCESS.
bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* engine, ID3D12Resource* post, ID3D12Resource* dlss, ID3D12Resource* out,
    uint32_t out_w, uint32_t out_h, uint32_t render_w, uint32_t render_h, float threshold, bool live_rule = false);

// The decision of a recent frame (k, r, match), read back every few seconds for the log; false if none is new.
bool latest(float& k, float& r, bool& match);

// Diagnostics: the log interval (setting dlss_guard_log, frames; 600 by default) and the post color's sum(s*s) of the
// last logged decision (0: the post color was black; with k 0 and no match the engine's image was black).
void set_log_every(uint32_t frames);
float last_post_energy();
bool last_live(); // the last logged decision was the live rule's
} // namespace upscale_guard
