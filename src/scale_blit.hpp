#pragma once

#include <d3d12.h>

#include <cstdint>

// A bilinear or bicubic (Catmull-Rom) scale of one texture into another (compute): RE7's videotape mode, DLSS's output at the tape's own
// resolution (2/3) up to the scene target, as the game's own step did.
namespace scale_blit {
bool init(ID3D12Device* device);

// `src` readable (NON_PIXEL_SHADER_RESOURCE), `dst` in UNORDERED_ACCESS; the states stay as they are. Sets the list's
// descriptor heaps, compute root signature and pipeline state (the caller restores the engine's).
bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst, uint32_t dst_w, uint32_t dst_h, bool bicubic);
} // namespace scale_blit
