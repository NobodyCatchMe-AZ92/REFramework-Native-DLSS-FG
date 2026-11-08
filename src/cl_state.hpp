#pragma once

#include <d3d12.h>

#include <memory>

// The state of the game's command lists that a compute pass recorded into the middle of one (DLSS) changes: pipeline
// state, descriptor heaps, compute root signature and arguments, graphics descriptor tables. Streamline can only
// restore this on command lists from its own proxy device; the game's are native, so it is tracked here (hooks on the
// command list functions, chained with other hooks) and put back after the pass.
namespace cl_state {
// Once, with the game's device: hooks the command list functions (the game's lists share them).
bool install(ID3D12Device* device);
bool installed();

// Diagnostics (setting perf_bypass): all command list hooks off/on live, to measure what they cost. While off, nothing
// is recorded into the game's lists (no DLSS); after switching on, a list is used again from its next Reset.
void set_hooks_enabled(bool enabled);

// The list's state before the pass (on the thread recording it), nullptr if not fully known (then don't record the pass),
// and putting it back after the pass (the pass's own calls go through the same hooks).
struct Saved;
struct SavedDeleter {
    void operator()(Saved* saved) const;
};
using SavedPtr = std::unique_ptr<Saved, SavedDeleter>;

SavedPtr save(ID3D12GraphicsCommandList* list);
void restore(ID3D12GraphicsCommandList* list, const Saved& saved);

// The ClearRenderTargetView hook (for the DLSS pass marker): called for every clear; returns true if it handled it.
using ClearHandler = bool (*)(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4]);
void set_clear_handler(ClearHandler handler);

// After the DLSS marker on `list` (this thread): when the engine next binds `scene_target_rtv` on it (its upscale), calls
// handler(list, the last resource it made a render target before that, context, false). REPLACE: the handler wrote the
// scene target, the engine's next `skip_draws` draws (its upscale) are skipped; the ones after them (sprites, logos the
// engine draws into the scene target in the same pass) still run. AFTER: the engine's draws run and the
// handler is called again at that next bind (after = true). AFTER_DRAWS: the engine's next `skip_draws` draws run and
// the handler is called again right after the last of them (after = true), before the engine's further draws.
// KEEP: nothing more.
enum UpscaleAction { KEEP, REPLACE, AFTER, AFTER_DRAWS };
using UpscaleHandler = UpscaleAction (*)(ID3D12GraphicsCommandList* list, ID3D12Resource* source, uint32_t context, bool after);
void arm(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE scene_target_rtv, UpscaleHandler handler, uint32_t context,
    uint32_t skip_draws);

// After arm() on `list`: right before the engine next transitions `resource` to RENDER_TARGET on it (before its upscale),
// calls handler(list, resource, the state it is in, context): its commands land before that barrier.
using WatchHandler = void (*)(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES state, uint32_t context);
void watch_render_target(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, WatchHandler handler, uint32_t context);

// After arm() on `list`: a compute upscale (RE4): a texture of `width` x `height` the engine makes an unordered access
// view while `source` (the HDR color) is out of the render target state (rendered into, then made readable or copied,
// in either order); right after the next Dispatch then, handler(list, that texture, context) (its commands land before
// the engine reads it). Once per arm(). The caller doesn't watch at native size (no upscale there; other compute passes
// write output-sized textures).
using ComputeHandler = void (*)(ID3D12GraphicsCommandList* list, ID3D12Resource* target, uint32_t context);
void watch_compute_upscale(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, ID3D12Resource* source, ComputeHandler handler);

// Inside the compute upscale handler: the texture the engine's pass read (the last one like `source` made readable before
// its dispatch; RE4: the scene's second color buffer, its post effects' result), or nullptr.
ID3D12Resource* compute_upscale_input();
D3D12_RESOURCE_STATES compute_upscale_input_state(); // its state at that point

// Diagnostics (probe_descriptors): logs the textures behind the graphics tables bound on `list` now.
void dump_graphics_bindings(ID3D12GraphicsCommandList* list, const char* label);

// The last texture made a render target on `list` since arm() (the upscale's source in the draw path), or nullptr.
ID3D12Resource* armed_last_render_target(ID3D12GraphicsCommandList* list);
D3D12_RESOURCE_STATES armed_last_render_target_state(ID3D12GraphicsCommandList* list); // its state now
// After arm() on `list`: calls handler(list, source, its state, context) once, right before the barrier batch that makes a
// texture of half the source's size (its format) a render target (RE3: the bloom chain's first draw, which reads the scene).
// `state`: the source's state at the call.
void watch_bloom_chain(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_RESOURCE_STATES state, WatchHandler handler, uint32_t context);

// Inside the draw path's upscale handler (the scene target's bind): the state its source (the last render target) is in.
D3D12_RESOURCE_STATES upscale_source_state();

// Diagnostics: logs the next `calls` draws/dispatches/barriers/copies/render target binds recorded on `list` (this thread).
void trace(ID3D12GraphicsCommandList* list, int calls);
} // namespace cl_state
