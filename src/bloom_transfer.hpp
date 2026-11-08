#pragma once

#include <d3d12.h>

#include <cstdint>

// RE4 (profile bloom_transfer): its compute upscale adds the bloom chain (240x135, 60x33, 30x16 at 1080p) while upscaling,
// and DLSS replaces that pass. Its own result is still in the output texture when the plugin runs (right after its
// dispatch): the difference between that result and a plain upscale of the same input, at 1/8 of the output size (bloom
// is smooth; sharpening and fine detail average out there), smoothly upscaled and added onto DLSS's image.
namespace bloom_transfer {
bool init(ID3D12Device* device);

// `engine` (the pass's output, output-sized, readable by compute shaders), `input` (its render-sized input, readable,
// render extent render_w x render_h), `dlss` (DLSS's output, readable). Writes DLSS + bloom into an internal texture and
// returns it (UNORDERED_ACCESS), or nullptr. `strength` scales the bloom (1 = as the game's). Sets the list's descriptor
// heaps, compute root signature and pipeline state (the caller restores the engine's).
ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* engine, ID3D12Resource* input, ID3D12Resource* dlss, uint32_t out_w,
    uint32_t out_h, uint32_t render_w, uint32_t render_h, float strength);
} // namespace bloom_transfer
