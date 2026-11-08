#pragma once

#include <d3d12.h>

#include <cstdint>

// A temporal stabilizer on DLSS's output for RE2's item examine view (profile examine_stabilizer). The examined item is
// bright fine detail on black; held still, DLSS leaves a small shimmer on its edges at the lower modes (measured 1.2% of
// pixels at Performance, 0.4% at DLAA). Where the engine's motion vectors (the item rotating) show no motion nearby, the
// output is blended with its own history (clamped to the current 3x3 neighbourhood: no ghosts); moving pixels pass
// through with DLSS's output and restart their history.
namespace examine_stabilizer {
bool init(ID3D12Device* device);

// `src` (output-sized, readable by compute shaders: NON_PIXEL_SHADER_RESOURCE), `motion` (render resolution, readable),
// `render_w/h` the render extent in it. Writes the stabilized image to an internal texture and returns it (left in
// UNORDERED_ACCESS), or nullptr. `reset`: start over (first frame of the view, a cut). Sets the list's descriptor heaps,
// compute root signature and pipeline state (the caller restores the engine's).
ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* motion, uint32_t out_w, uint32_t out_h,
    uint32_t render_w, uint32_t render_h, float blend, bool reset);

// A frame without the pass: the history starts over next time.
void invalidate();
} // namespace examine_stabilizer
