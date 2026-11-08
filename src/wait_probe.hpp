#pragma once

#include <d3d12.h>

#include <string>

// Diagnostics (setting perf_waits, live): how long the game's threads wait per frame, and on what: the swapchain's frame
// latency waitable, GPU fences (events given to ID3D12Fence::SetEventOnCompletion), or anything else (the game's own
// thread synchronization). Hooks on KernelBase's WaitForSingleObject(Ex) / WaitForMultipleObjects(Ex), installed the first
// time it is switched on, and switched off again with the setting.
namespace wait_probe {
void set_enabled(bool enabled, ID3D12Device* device);
void set_waitable(HANDLE waitable);  // the swapchain's frame latency waitable the game got
void on_present_thread();            // called on the thread that presents (the render thread)
void on_frame();                     // once per presented game frame
std::string status();                // one line for the log ("" when off)
} // namespace wait_probe
