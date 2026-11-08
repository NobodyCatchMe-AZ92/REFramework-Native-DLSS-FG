#pragma once

#include <d3d12.h>

#include <cstdint>

// The plugin's bloom after DLSS (profile own_bloom, RE8). The game's (via.render.SoftBloom, algorithm CoDAW) runs on the
// render-resolution scene before the upscale: DLSS then got a glow that flickered with the jitter on small bright spots.
// With DLSS on the game's output ratio is zeroed and this one builds the same kind of pyramid from DLSS's full-resolution
// output (CoD: Advanced Warfare: 13-tap downsamples, the first one brightness-weighted against single-pixel fireflies,
// tent-filtered upsamples), with the game's threshold, dispersion and output ratio.
namespace own_bloom {
struct Params {
    float threshold{};   // SoftBloom.Threshold
    float dispersion{};  // SoftBloom.Dispersion: how much of the coarser levels each upsample takes (0..1)
    float ratio{};       // SoftBloom.OutputRatio: the bloom's share of the result
    float strength{1.0f}; // setting dlss_own_bloom_strength (tuning)
    float inner{};       // setting dlss_own_bloom_inner: extra weight of the finest levels (the glow close to a light)
    float threshold_override{-1.0f}; // setting dlss_own_bloom_threshold: >= 0 replaces the game's (in DLSS's image's units, not scaled)
    bool additive{true}; // setting dlss_own_bloom_add: scene + bloom * ratio (default) instead of lerp(scene, bloom, ratio)
};

bool init(ID3D12Device* device);

// `src` (output-sized, readable by compute shaders). The game's threshold is in the units of its HDR scene (`hdr`, before
// its post effects), DLSS's image is its post effects' result (`post`, DLSS's input, exposure applied): their brightness
// ratio is measured on the GPU each frame (smoothed) and the threshold scaled by it. `hdr` / `post` (render resolution) in
// the given states, put back. Writes into an internal texture and returns it (UNORDERED_ACCESS), or nullptr. Sets the list's
// descriptor heaps, compute root signature and pipeline state (the caller restores the engine's).
ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* hdr, D3D12_RESOURCE_STATES hdr_state,
    ID3D12Resource* post, D3D12_RESOURCE_STATES post_state, uint32_t out_w, uint32_t out_h, const Params& params);
} // namespace own_bloom
