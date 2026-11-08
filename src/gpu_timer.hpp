#pragma once

#include <d3d12.h>

#include <string>

// GPU time of the plugin's own passes (timestamp queries), averaged between status lines in the log, to compare their
// cost across DLSS modes. Each pass is timed with begin/end on the command list it is recorded into.
namespace gpu_timer {
enum Pass { BLOOM_STABILIZER, DLSS_EVALUATE, UPSCALE_GUARD, DEPTH_OF_FIELD, PASS_COUNT };

bool init(ID3D12Device* device);
void next_frame(); // once per frame (at the DLSS marker): reads back the frames old enough to be done
void begin(ID3D12GraphicsCommandList* list, Pass pass);
void end(ID3D12GraphicsCommandList* list, Pass pass);
std::string report(); // "GPU ms: ..." averages since the last report, then reset
} // namespace gpu_timer
