#pragma once

#include <d3d12.h>

#include <cstdint>

#include "mat4.hpp"

// The plugin's own depth of field (profile own_dof, RE7), at the output resolution on DLSS's image.
//
// RE7's depth of field runs at render resolution inside DLSS's input: it shimmers (it relies on the game's TAA, which DLSS
// replaces), and below about a pixel of its half-resolution buffers it drops the blur, so at lower DLSS modes the subtle
// background blur vanishes (measured: no visible blur at Performance, even without DLSS). While DLSS is on, the game's
// depth of field is neutralised for rendering (an extreme aperture at BeginRendering, after the game wrote its values for the
// frame) and the same lens (its live aperture, focus distance, sensor size, maximum blur) is applied here after DLSS:
// thin-lens circle of confusion from the depth, relative to the screen (the same in every DLSS mode), gathered with a
// golden-angle spiral; farther samples can't spread more blur onto a nearer pixel than it has itself.
namespace own_dof {
// BeginRendering (game thread): reads the game's depth of field values and, if `take_over`, neutralises its blur
// for rendering. Without take_over the game's own depth of field stays.
void on_begin_rendering(bool take_over);

// Diagnostics (setting probe_dof, any game): read-only; logs when the game's depth of field switches on/off and when its
// aperture or focus changes noticeably (finding scenes with depth of field).
void probe();

// Whether this frame gets the plugin's depth of field (the game wants depth of field and it was taken over).
bool active();

// RE2's item examine: the game's depth of field on with BokehControl IgnoreNear (its only user found so far: focus 30 m,
// a 10x sensor; the inventory itself uses Default, gameplay IgnoreFar). Read at BeginRendering.
bool examining();
// RE3: the examine view known from its GUI component (game thread, every 10th frame): examining() is true while set.
void set_examine_hint(bool examining);

bool init(ID3D12Device* device);

// `src` (DLSS's output, output-sized) readable (NON_PIXEL_SHADER_RESOURCE), `depth` and `motion` (the engine's, render
// resolution, readable by compute shaders), `dst` output-sized in UNORDERED_ACCESS. `projection`: the camera's (unjittered).
// `jitter_px`: experiment, a shift of the depth lookup (render pixels; 0 = none). `strength`: a scale on the circle of
// confusion (1 = the lens as the game set it). `temporal_blend`: this frame's weight in the smoothed circle of confusion
// (1 = no history); `reset`: drop the history (camera cut). Sets the list's descriptor heaps, compute root signature and
// pipeline state (the caller restores the engine's).
bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* dst,
    uint32_t out_w, uint32_t out_h, uint32_t render_w, uint32_t render_h, const Mat4& projection, const float jitter_px[2], float strength, float max_scale,
    float temporal_blend, bool reset);

// A frame without the pass: the smoothed circle of confusion starts over next time.
void invalidate_history();
} // namespace own_dof
