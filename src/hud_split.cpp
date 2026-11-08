#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "cl_state.hpp"
#include "dlss_pass.hpp"
#include "framegen.hpp"
#include "log.hpp"
#include "refw.hpp"
#include "scene_inputs.hpp"

#include "hud_split.hpp"

using Microsoft::WRL::ComPtr;

namespace {
enum Kind : uint32_t { OVERLAY_PRE = 1, PREPARE_OUTPUT_PRE = 2, PREPARE_OUTPUT_POST = 3 };

constexpr uint32_t RING = 3; // DLSS-G reads the HUD-less frame at Present, later, on its own queue
constexpr auto HUDLESS_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12Resource> g_with_hud{};       // the scene target with the HUD, while PrepareOutput draws the HUD-free one
ComPtr<ID3D12Resource> g_hudless[RING]{};  // the HUD-less frames (backbuffer copies)
std::vector<ComPtr<ID3D12Resource>> g_retired{};
uint32_t g_next{};

// Set on the thread recording the frame's command list (DLSS pass, then the PrepareOutput markers).
std::atomic<ID3D12Resource*> g_hudless_scene{};
bool g_swapped{};

std::atomic<uint32_t> g_frame{};
std::atomic<uint32_t> g_made{};
std::atomic<uint32_t> g_made_total{}; // never reset (the status file compares it second to second)
std::atomic<uint32_t> g_missed{};
std::atomic<bool> g_paused{};

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

// Diagnostics (dlss_debug=8): one HUD-less frame saved as reframework/data/refw_framegen_hudless.bmp.
ComPtr<ID3D12Resource> g_snapshot{};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_snapshot_footprint{};
std::atomic<int> g_snapshot_frames{-1};
std::atomic<bool> g_snapshot_requested{}; // F9 with dlss_debug=8

void record_snapshot(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
    const auto desc = texture->GetDesc();
    UINT rows{};
    UINT64 row_size{};
    UINT64 total{};
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &g_snapshot_footprint, &rows, &row_size, &total);

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_snapshot)))) {
        return;
    }

    barrier(list, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{g_snapshot.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = g_snapshot_footprint;
    D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    g_snapshot_frames = 1;
    log_info("HUD fix: snapshot of the HUD-less frame queued ({}x{} format {})", desc.Width, desc.Height, (int)desc.Format);
}

void save_snapshot() {
    const auto left = g_snapshot_frames.load();

    if (left < 0) {
        return;
    }

    if (left > 0) {
        g_snapshot_frames = left - 1;
        return;
    }

    g_snapshot_frames = -1;
    uint8_t* data{};

    if (g_snapshot == nullptr || FAILED(g_snapshot->Map(0, nullptr, (void**)&data))) {
        return;
    }

    const auto w = g_snapshot_footprint.Footprint.Width;
    const auto h = g_snapshot_footprint.Footprint.Height;
    const auto pitch = g_snapshot_footprint.Footprint.RowPitch;
    std::vector<uint8_t> bmp(54 + (size_t)w * h * 3);
    const auto size = (uint32_t)bmp.size();
    bmp[0] = 'B';
    bmp[1] = 'M';
    std::memcpy(&bmp[2], &size, 4);
    const uint32_t offset = 54, header = 40, pixels = (uint32_t)(w * h * 3);
    const int32_t width = (int32_t)w, height = -(int32_t)h;
    const uint16_t planes = 1, bits = 24;
    std::memcpy(&bmp[10], &offset, 4);
    std::memcpy(&bmp[14], &header, 4);
    std::memcpy(&bmp[18], &width, 4);
    std::memcpy(&bmp[22], &height, 4);
    std::memcpy(&bmp[26], &planes, 2);
    std::memcpy(&bmp[28], &bits, 2);
    std::memcpy(&bmp[34], &pixels, 4);

    for (UINT y = 0; y < h; ++y) {
        const auto row = data + g_snapshot_footprint.Offset + (UINT64)y * pitch;

        for (UINT x = 0; x < w; ++x) {
            auto out = &bmp[54 + ((size_t)y * w + x) * 3];
            out[0] = row[x * 4 + 2]; // RGBA8 -> BGR
            out[1] = row[x * 4 + 1];
            out[2] = row[x * 4 + 0];
        }
    }

    g_snapshot->Unmap(0, nullptr);
    g_snapshot.Reset();

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path{exe};
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"reframework\\data\\refw_framegen_hudless.bmp";

    if (const auto file = _wfopen(path.c_str(), L"wb")) {
        fwrite(bmp.data(), 1, bmp.size(), file);
        fclose(file);
        log_info("HUD fix: HUD-less frame saved ({}x{})", w, h);
    }
}

// The main scene if `layer` (being drawn, so alive) belongs to it, else nullptr.
void* main_scene_of(void* layer) {
    static const auto get_parent = refw::find_method("via.render.RenderLayer", "get_Parent");
    const auto scene = layer != nullptr ? (void*)refw::call(get_parent, layer) : nullptr;
    return scene != nullptr && scene == scene_inputs::main_scene() ? scene : nullptr;
}

// The scene target and its view, taken from the live scene while PrepareOutput draws and used when the markers are
// recorded: never looked up again through the remembered scene address, which goes stale when the engine recreates its
// scenes (a resolution change crashed the game that way).
std::atomic<ID3D12Resource*> g_target{};
std::atomic<void*> g_target_rtv{};

void capture_target(void* scene) {
    g_target = dlss_pass::scene_target(scene);
    g_target_rtv = dlss_pass::scene_target_rtv(scene);
}

bool wanted() {
    const auto dlss = framegen::dlss();
    return dlss != nullptr && dlss->hud_fix && dlss->fg_enabled && !g_paused && g_device != nullptr;
}

bool recon_frame() {
    const auto dlss = framegen::dlss();
    const auto frame = g_frame.load();
    return dlss != nullptr && dlss->debug == 7 && (frame == 600 || frame == 601);
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

void copy(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, D3D12_RESOURCE_STATES dst_state, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state) {
    barrier(list, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(list, dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(dst, src);
    barrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
    barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
}

bool ensure(ComPtr<ID3D12Resource>& texture, const D3D12_RESOURCE_DESC& like, D3D12_RESOURCE_STATES state, const wchar_t* name) {
    if (texture != nullptr) {
        const auto desc = texture->GetDesc();

        if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
            return true;
        }

        g_retired.push_back(texture);
        texture.Reset();

        if (g_retired.size() > 8) {
            g_retired.erase(g_retired.begin());
        }
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = like.Width;
    desc.Height = like.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = like.Format;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture)))) {
        log_error("HUD fix: could not create {}x{} texture (format {})", like.Width, like.Height, (int)like.Format);
        return false;
    }

    texture->SetName(name);
    return true;
}

void queue(void* render_context, Kind kind) {
    dlss_pass::queue_marker(render_context, g_target_rtv.load(), 2000.0f + (float)kind);
}

// Before PrepareOutput: the scene target (with the HUD, a render target for the marker) is saved, the HUD-free scene put in.
void before_prepare_output(ID3D12GraphicsCommandList* list) {
    g_swapped = false;
    const auto hudless_scene = g_hudless_scene.exchange(nullptr);
    const auto target = g_target.load();

    if (hudless_scene == nullptr || target == nullptr) {
        ++g_missed;
        return;
    }

    const auto desc = target->GetDesc();
    const auto scene_desc = hudless_scene->GetDesc();

    if (scene_desc.Width != desc.Width || scene_desc.Height != desc.Height || scene_desc.Format != desc.Format ||
        !ensure(g_with_hud, desc, HUDLESS_STATE, L"refw_framegen scene with HUD"))
    {
        ++g_missed;
        return;
    }

    copy(list, g_with_hud.Get(), HUDLESS_STATE, target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    copy(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, hudless_scene, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_swapped = true;
}

// After the first PrepareOutput draw: the backbuffer (still a render target) holds the HUD-less frame; then the scene with
// the HUD goes back for the second draw.
void after_prepare_output(ID3D12GraphicsCommandList* list) {
    if (!g_swapped) {
        return;
    }

    g_swapped = false;
    const auto target = g_target.load();
    const auto backbuffer = framegen::current_backbuffer();

    if (target != nullptr) {
        copy(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, g_with_hud.Get(), HUDLESS_STATE);
    }

    if (backbuffer == nullptr) {
        ++g_missed;
        return;
    }

    const auto i = g_next++ % RING;

    if (!ensure(g_hudless[i], backbuffer->GetDesc(), HUDLESS_STATE, L"refw_framegen HUD-less frame")) {
        ++g_missed;
        return;
    }

    copy(list, g_hudless[i].Get(), HUDLESS_STATE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (g_snapshot_requested.exchange(false)) {
        record_snapshot(list, g_hudless[i].Get(), HUDLESS_STATE);
    }

    framegen::set_hudless(g_hudless[i].Get(), (uint32_t)HUDLESS_STATE);
    ++g_made;
    ++g_made_total;
}
} // namespace

namespace hud_split {
void on_device(ID3D12Device* device) {
    g_device = device;
}

void on_overlay_draw_pre(void* layer, void* render_context) {
    const auto scene = main_scene_of(layer);

    if (scene == nullptr) {
        return;
    }

    capture_target(scene);

    ++g_frame;

    if (recon_frame()) {
        queue(render_context, OVERLAY_PRE);
    }

    // Diagnostics (dlss_debug=8): F9 saves the next HUD-less frame.
    if (const auto dlss = framegen::dlss(); dlss != nullptr && dlss->debug == 8) {
        static bool was_down = false;
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const auto down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

        if (down && !was_down) {
            g_snapshot_requested = true;
        }

        was_down = down;
    }

    // The F5 key (setting hud_fix_key) switches the HUD fix off and on live, to compare.
    if (const auto dlss = framegen::dlss(); dlss != nullptr && dlss->hud_fix_key == 0) {
        if (g_paused.exchange(false)) {
            log_info("HUD fix: ON (diagnostic hotkeys off)");
        }
    } else if (dlss != nullptr) {
        static bool was_down = false;
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const auto down = pid == GetCurrentProcessId() && (GetAsyncKeyState(dlss->hud_fix_key) & 0x8000) != 0;

        if (down && !was_down) {
            const auto paused = !g_paused.exchange(!g_paused.load());
            log_info("HUD fix: {} (key)", paused ? "OFF" : "ON");
        }

        was_down = down;
    }
}

void prepare_output_draw(void* layer, void* render_context, DrawFn draw) {
    const auto scene = main_scene_of(layer);
    const auto main = scene != nullptr;
    const auto split = main && wanted();

    if (main) {
        capture_target(scene);
    }

    if (main && recon_frame()) {
        queue(render_context, PREPARE_OUTPUT_PRE);
        draw(layer, render_context);
        queue(render_context, PREPARE_OUTPUT_POST);
        return;
    }

    if (!split) {
        g_hudless_scene = nullptr;
        draw(layer, render_context);
        return;
    }

    queue(render_context, PREPARE_OUTPUT_PRE);
    draw(layer, render_context); // the HUD-less frame (if the scene was swapped)
    queue(render_context, PREPARE_OUTPUT_POST);
    draw(layer, render_context); // the real frame
}

void set_hudless_scene(ID3D12Resource* scene) {
    g_hudless_scene = scene;
}

bool on_marker(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t kind) {
    if (recon_frame()) {
        if (kind == OVERLAY_PRE) {
            log_info("trace: ---- Overlay marker on list {:x} (rtv {:x}); scene target {:x}; backbuffers {}", (uintptr_t)list, rtv.ptr,
                (uintptr_t)g_target.load(), framegen::describe_backbuffers());
            cl_state::trace(list, 300);
        } else {
            log_info("trace: ==== PrepareOutput {} marker on list {:x} ====", kind == PREPARE_OUTPUT_PRE ? "pre" : "post", (uintptr_t)list);
        }

        return true;
    }

    if (kind == PREPARE_OUTPUT_PRE) {
        before_prepare_output(list);
    } else if (kind == PREPARE_OUTPUT_POST) {
        after_prepare_output(list);
    }

    return true;
}

std::string status() {
    save_snapshot();
    return std::format("HUD fix: {} HUD-less frames, {} missed{}", g_made.exchange(0), g_missed.exchange(0), g_paused ? " (OFF by key)" : "");
}
} // namespace hud_split

namespace hud_split {
std::string status_values() {
    static uint32_t last_total = 0;
    const auto total = g_made_total.load();
    const auto active = total != last_total;
    last_total = total;
    return std::format("hud_fix_active={}\n", active ? 1 : 0);
}
} // namespace hud_split
