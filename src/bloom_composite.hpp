#pragma once

#include <d3d12.h>

#include <cstdint>

// Keeps what the engine does after DLSS's input (bloom, its exposure, extras) out of DLSS:
//   out = bilinear(post) + bilinear(post / lit) * (upscaled - bilinear(pre))
// upscaled: DLSS's output (full resolution) from `pre`, the scene before the engine's bloom (render resolution);
// lit: the scene with the bloom added; post: the engine's post pass result from it (render resolution). If the post
// pass is exposure * (scene + bloom) + extras, out is exposure * (upscaled + bloom) + extras, and the jitter cancels.
// A compute shader recorded into the game's command list (the caller saves and restores the list's state).
namespace bloom_composite {
bool init(ID3D12Device* device);

// States on entry and exit: upscaled, post, pre and lit readable by compute shaders, out in UNORDERED_ACCESS.
bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* upscaled, ID3D12Resource* post, ID3D12Resource* pre, ID3D12Resource* lit, ID3D12Resource* out,
    uint32_t out_w, uint32_t out_h, uint32_t render_w, uint32_t render_h);
} // namespace bloom_composite
