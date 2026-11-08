#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

// The main scene's render inputs (for DLSS and DLSS-G): its depth buffer, motion vectors and the color it hands to
// PrepareOutput, found every frame through the render layer hooks.
namespace scene_inputs {
// Layer hooks (render thread).
void on_scene_update(void* scene_layer);
void on_prepare_output_draw(void* prepare_output_layer); // after the layer drew

struct Inputs {
    void* scene_layer{};
    ID3D12Resource* depth{};
    ID3D12Resource* motion{};
    ID3D12Resource* color{};
    uint32_t frame{}; // increments each time the inputs are refreshed
};

Inputs latest();
void* main_scene(); // the main scene layer (from this frame's Scene update)
std::string describe(); // one line for the status log
} // namespace scene_inputs
