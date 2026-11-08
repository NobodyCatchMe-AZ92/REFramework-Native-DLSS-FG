#pragma once

#include <d3d12.h>

#include <cstdint>

// AMD FidelityFX Contrast Adaptive Sharpening (CAS, MIT license: THIRD_PARTY_NOTICES) on DLSS's output (setting dlss_cas,
// menu "Enable FidelityFX CAS Sharpening", on by default; strength dlss_cas_strength). Every game.
//
// DLSS's output here is the scene in HDR, before the game's tonemapping, post effects and HUD. CAS expects 0..1 values, so
// per pixel the 3x3 neighbourhood is scaled so its brightest channel is 0.5 (CAS then depends only on the local contrast,
// not on the scene's exposure, and has headroom), taken through an approximate gamma (square root: sharpening as on the
// displayed image, as ReShade's CAS does), sharpened, and taken back. Strength: as ReShade's CAS "Sharpening" (the blend
// of the sharpened result over the original), contrast adaptation at CAS's default.
namespace cas_sharpen {
bool init(ID3D12Device* device);

// `src` (readable by compute shaders: NON_PIXEL_SHADER_RESOURCE) -> an internal texture like it, returned in
// UNORDERED_ACCESS, or nullptr. Sets the list's descriptor heaps, compute root signature and pipeline state (the caller
// restores the engine's).
ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, uint32_t width, uint32_t height, float strength);
} // namespace cas_sharpen
