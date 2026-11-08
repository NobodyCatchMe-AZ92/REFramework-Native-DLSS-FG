#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

// The HUD-less frame for frame generation (DLSS-G's hudless color). RE2's Overlay layer draws the scene into the scene
// target (the quad DLSS replaces), then composites the whole GUI over it with a second quad; PrepareOutput then draws the
// scene target into the swapchain's backbuffer. So, with markers at PrepareOutput:
//   before it: the scene target (with the HUD) is saved and the HUD-free scene (DLSS's result) put in its place;
//   PrepareOutput draws once: the backbuffer holds the HUD-less frame, copied for DLSS-G;
//   then the saved scene goes back and PrepareOutput draws again: the real frame.
namespace hud_split {
void on_device(ID3D12Device* device);

// Layer hooks (render thread). The PrepareOutput hook draws the layer itself (`draw` = the original), twice when needed.
void on_overlay_draw_pre(void* layer, void* render_context);
using DrawFn = void (*)(void* layer, void* render_context);
void prepare_output_draw(void* layer, void* render_context, DrawFn draw);

// The DLSS pass's result this frame (the scene without the HUD, in UNORDERED_ACCESS, full size), recorded before the GUI.
void set_hudless_scene(ID3D12Resource* scene);

// A HUD marker reached a command list (kind from the marker); returns true (markers are never real clears).
bool on_marker(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t kind);

std::string status(); // for the status line (resets the counters)
std::string status_values(); // for the status file: "hud_fix_active=0/1" (HUD-less frames made in the last second)
} // namespace hud_split
