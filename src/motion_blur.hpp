#pragma once

#include <d3d12.h>

#include <cstdint>

// The plugin's motion blur after DLSS (profile own_motion_blur, RE7). The game's own runs at the render resolution before
// the upscale: DLSS then got a scene smeared differently each frame (foliage flickered badly while moving). With DLSS on
// the engine's is switched off and, if the game's option is on, this one blurs DLSS's output: along each pixel's motion
// (the engine's velocity, camera and objects), the shutter angle's share of the frame's movement, centred; samples whose
// own motion differs a lot from this pixel's (another object) are left out.
namespace motion_blur {
bool init(ID3D12Device* device);

// `src` (output-sized, readable by compute shaders), `motion` (render resolution, readable), render extent render_w x
// render_h in it. `shutter`: the share of the frame's movement blurred (ShutterAngle / 360). Writes into an internal
// texture and returns it (UNORDERED_ACCESS), or nullptr. Sets the list's descriptor heaps, compute root signature and
// pipeline state (the caller restores the engine's).
ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* motion, uint32_t out_w, uint32_t out_h,
    uint32_t render_w, uint32_t render_h, float shutter);
} // namespace motion_blur
