#pragma once

#include <d3d12.h>

#include <cstdint>

// A steady input for the engine's bloom. RE2 computes its bloom from the scene at render resolution, before the upscale:
// with DLSS's jitter (and no TAA of the game's own) thin bright lights fall on different pixels every frame and the bloom
// of them flickers, the more the lower the render resolution. This is a small TAA for the bloom's input only:
//   out = lerp(clamp(history(reprojected by the motion vectors), 3x3 neighborhood, widened), current, alpha)
// The caller puts `out` into the scene for the bloom chain to read and restores the original before the bloom is added
// to it, so DLSS still gets the jittered scene (with a steady bloom on it).
// A compute shader recorded into the game's command list (the caller saves and restores the list's state).
namespace bloom_stabilizer {
bool init(ID3D12Device* device);

// States on entry and exit: current, history and motion readable by compute shaders, out in UNORDERED_ACCESS.
// mvec_scale: motion vector -> UV offset to the previous frame; reset: no history (cut, first frame, new size).
// moving_px: pixels moving more than this (render pixels per frame, 3x3) are passed through (RE2 0.1: its Model Viewer;
// RE3 1.0: its camera sways a little even standing still, which left nothing stabilized).
bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* current, ID3D12Resource* history, ID3D12Resource* motion, ID3D12Resource* out,
    uint32_t width, uint32_t height, const float mvec_scale[2], bool reset, float moving_px = 0.1f, float alpha = 0.1f);
} // namespace bloom_stabilizer
