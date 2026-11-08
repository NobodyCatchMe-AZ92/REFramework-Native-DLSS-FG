#pragma once

#include <d3d12.h>

#include <cstdint>

// RE2 only, EXPERIMENTAL (menu: Advanced, off by default): against the fixed dither pattern of hair and some shadows.
//
// Research (development notes 2026-11-08): the dither is fixed to the render-pixel grid. Nothing per frame drives it (no counter in the
// scene constants; writing the plugin's jitter into the engine's jitter values changed nothing), and the game's own TAA
// doesn't resolve it either, it only blurs over it. At Performance one render pixel covers 2x2 output pixels: it shows.
//
// The approach: the scene is rendered moved by whole render pixels in a small repeating cycle (only the projection gets that
// offset), so the fixed pattern falls on a different part of the surface each frame; before DLSS, its inputs (color, depth,
// motion vectors) are moved back by the same whole pixels. DLSS gets ordinary inputs with its usual sub-pixel jitter and can
// average the now varying dither like real transparency.
//   A 4x4 loop (16 frames) moving one pixel per frame (a closed loop through all 16 positions). The menu: one checkbox,
//   "[EXPERIMENTAL] Hair/shadow dither fix (RE2)", off by default. (Research modes: 2x2, one axis only, 6x6, 8x8; development notes.)
//   - The engine's previous matrices get last frame's shift: its motion vectors hold the shift's change, so its own temporal
//     passes (the ray-traced reflection denoiser, the plugin's bloom stabilizer) follow the moved picture. DLSS's copy of the
//     motion vectors has the change taken out again (still pixels snapped to exactly 0).
//   - The one-pixel steps matter: with jumps of 3 (the last column back to the first) edges in the game's own image jumped
//     now and then.
//   - The strip along the edges the picture moved away from (up to 3 render pixels, nothing rendered there) comes from DLSS's
//     previous output (else the repeated last row flickered). Limit: while the camera moves fast, that strip lags a little.
//   - After DLSS the engine's motion vectors get DLSS's moved-back copy (its passes after the upscale work on DLSS's output).
//
// Tried and dropped (development notes): telling DLSS the whole pixels as jitter (black lines / dots: it expects jitter within half a
// pixel); no correction at all (reflections jittered, then rotated with the cycle); the shift's change added to the engine's
// finished motion vectors instead of its matrices (edges jittered every frame: matrices and motion vectors disagreed); the
// offset in the engine's own TAA jitter values (ignored with its TAA off).
//
// Isolation: gated by the RE2 profile switch dither_experiments; mode() is 0 in every other game (and in RE2 by default), the
// shift is then 0 and the calls in the shared code (jitter and previous matrices, DLSS inputs, motion vectors after DLSS,
// frame generation's copies) change nothing.
namespace re2_dither {
// 1 = on (the 4x4 loop; setting dlss_dither_test != 0), 0 = off (the default, and always outside RE2).
int mode();

// This frame's whole-pixel shift (render pixels, as added to the projection), {0, 0} when off.
void pixel_shift(uint32_t frame, int shift[2]);

// Where a projection shift moves the picture: the offset added to row 2 is divided by w = z * m[2][3] (RE Engine's camera is
// right-handed, m[2][3] = -1: +s pixels in the projection moves the picture left / up). Render pixels, x right / y down.
void picture_moved(const int shift[2], float projection_m23, int moved[2]);

// DLSS's inputs for a frame whose picture moved by `moved`: each copied moved back (compute passes recorded into `list`; the
// caller saves and restores the list's state). On success the three are replaced by the copies (NON_PIXEL_SHADER_RESOURCE).
struct Inputs {
    ID3D12Resource* color;
    D3D12_RESOURCE_STATES color_state;
    ID3D12Resource* depth;
    D3D12_RESOURCE_STATES depth_state;
    ID3D12Resource* motion;
    D3D12_RESOURCE_STATES motion_state;
};

// motion_change: the shift's change in the engine's motion vectors (clip units; the previous matrices carry last frame's
// shift), taken out for DLSS. previous_output: DLSS's last output (in previous_state), for the color's strip along the
// edges the picture moved away from (nothing rendered there); nullptr (a cut, a reset): the last row / column repeated.
bool unshift(ID3D12GraphicsCommandList* list, ID3D12Device* device, Inputs& inputs, const int moved[2], const float motion_change[2],
    ID3D12Resource* previous_output, D3D12_RESOURCE_STATES previous_state, uint32_t render_w, uint32_t render_h);

// After DLSS: the engine's motion vectors replaced by the moved-back copy, for its passes after the upscale (they work on
// DLSS's output, which isn't moved).
void restore_motion(ID3D12GraphicsCommandList* list, ID3D12Resource* engine_motion, D3D12_RESOURCE_STATES state);

} // namespace re2_dither
