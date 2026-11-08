#pragma once

#include <d3d12.h>

// The frame latency waitable the game waits on before each frame, made by the plugin.
//
// Through Streamline's swapchain proxy the game's waitable belongs to Streamline's internal swapchain: it is signaled only
// after Streamline has copied the frame into its own back buffer and presented it, even with frame generation off. RE4
// (maximum frame latency 1) waited 3.2 ms per frame on it at native, 4.1 in Performance (78 fps against 99 with a normal
// swapchain), and SetMaximumFrameLatency through the proxy didn't change that.
//
// Here instead: a semaphore released when the GPU has finished each presented frame's work on the game's queue (a fence
// signaled at Present, waited for by a thread of the plugin). That is what a normal swapchain's waitable does (the frame
// before is done), so the game's latency setting means what it means without Streamline.
namespace frame_pacer {
bool init(ID3D12Device* device, ID3D12CommandQueue* queue);
bool ready();

HANDLE waitable();                // the semaphore the game gets as its frame latency waitable
void set_max_latency(int frames); // the game's SetMaximumFrameLatency (or the override): frames in flight
int max_latency();
void on_present();                // on the presenting thread, before the swapchain's Present: this frame's fence
} // namespace frame_pacer
