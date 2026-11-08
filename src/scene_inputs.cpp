#include <atomic>
#include <format>
#include <mutex>

#include "log.hpp"
#include "pipeline_probe.hpp"
#include "refw.hpp"
#include "render_objects.hpp"

#include "scene_inputs.hpp"
#include "framegen.hpp"

#include <map>
#include <windows.h>

namespace {
std::mutex g_mutex{};
scene_inputs::Inputs g_inputs{};
std::atomic<void*> g_main_scene{};

struct Methods {
    REFrameworkMethodHandle main_view{};      // via.SceneManager.get_MainView
    REFrameworkMethodHandle primary_camera{}; // via.SceneView.get_PrimaryCamera
    REFrameworkMethodHandle layer_camera{};   // via.render.layer.Scene.get_Camera
    REFrameworkMethodHandle layer_enabled{};  // via.render.layer.Scene.get_Enable
    REFrameworkMethodHandle layer_mirror{};   // via.render.layer.Scene.get_Mirror
    REFrameworkMethodHandle layer_parent{};   // via.render.RenderLayer.get_Parent
    bool resolved{};
} g_methods{};

void resolve_methods() {
    if (g_methods.resolved) {
        return;
    }

    g_methods.main_view = refw::find_method("via.SceneManager", "get_MainView");
    g_methods.primary_camera = refw::find_method("via.SceneView", "get_PrimaryCamera");
    g_methods.layer_camera = refw::find_method("via.render.layer.Scene", "get_Camera");
    g_methods.layer_enabled = refw::find_method("via.render.layer.Scene", "get_Enable");
    g_methods.layer_mirror = refw::find_method("via.render.layer.Scene", "get_Mirror");
    g_methods.layer_parent = refw::find_method("via.render.RenderLayer", "get_Parent");
    g_methods.resolved = true;

    if (g_methods.main_view == nullptr || g_methods.primary_camera == nullptr || g_methods.layer_camera == nullptr ||
        g_methods.layer_enabled == nullptr || g_methods.layer_mirror == nullptr || g_methods.layer_parent == nullptr)
    {
        log_error("Scene inputs: some type database methods are missing (main view/camera/layer): inputs unavailable");
    }
}

void* primary_camera() {
    const auto scene_manager = refw::native_singleton("via.SceneManager");
    const auto main_view = scene_manager != nullptr ? (void*)refw::call(g_methods.main_view, scene_manager) : nullptr;
    return main_view != nullptr ? (void*)refw::call(g_methods.primary_camera, main_view) : nullptr;
}

// The scene color is what PrepareOutput reads: its first shader resource view that leads to a texture (its target state
// is its output). The offset is found once.
void* scene_color_target(void* prepare_output_layer) {
    static size_t offset = 0;
    const auto base = (const uint8_t*)prepare_output_layer;

    if (offset != 0) {
        return *(void* const*)(base + offset);
    }

    for (size_t o = sizeof(void*); o < 0x200; o += sizeof(void*)) {
        const auto member = *(void* const*)(base + o);

        if (render_objects::type_name(member) == "via.render.ShaderResourceView" && render_objects::d3d12_resource(member) != nullptr) {
            offset = o;
            log_info("via.render.layer.PrepareOutput -> scene color view at +{:x}", o);
            return member;
        }
    }

    return nullptr;
}

std::string describe_resource(ID3D12Resource* resource) {
    if (resource == nullptr) {
        return "none";
    }

    const auto desc = resource->GetDesc();
    return std::format("{}x{} fmt {}", desc.Width, desc.Height, (int)desc.Format);
}
} // namespace

namespace scene_inputs {
void on_scene_update(void* scene_layer) {
    resolve_methods();

    // Diagnostics (dlss_debug=12): every scene layer once every 3 s (RE2's item examine: where is the item rendered?).
    if (const auto d = framegen::dlss(); d != nullptr && d->debug == 12) {
        static std::map<void*, ULONGLONG> last{};
        const auto now = GetTickCount64();

        if (auto& t = last[scene_layer]; now - t >= 3000) {
            t = now;
            const auto cam = (void*)refw::call(g_methods.layer_camera, scene_layer);
            const auto hdr = render_objects::d3d12_resource(refw::reflection_pointer(scene_layer, "HDRTarget"));
            const auto depth = render_objects::d3d12_resource(refw::reflection_pointer(scene_layer, "DepthStencilTex"));
            const auto vel = render_objects::d3d12_resource(refw::reflection_pointer(scene_layer, "VelocityTarget"));
            log_info("layers: scene {:x}{} enabled {} mirror {} camera {:x}{} hdr {} depth {} velocity {}", (uintptr_t)scene_layer,
                scene_layer == g_main_scene.load() ? " (main)" : "", refw::call(g_methods.layer_enabled, scene_layer) & 0xFF,
                refw::call(g_methods.layer_mirror, scene_layer) & 0xFF, (uintptr_t)cam, cam == primary_camera() ? " (primary)" : "",
                describe_resource(hdr), describe_resource(depth), describe_resource(vel));
        }
    }

    // The main scene: enabled, not a mirror, rendered with the game's primary camera.
    const auto camera = (void*)refw::call(g_methods.layer_camera, scene_layer);

    if (camera == nullptr || camera != primary_camera() || (refw::call(g_methods.layer_enabled, scene_layer) & 0xFF) == 0 ||
        refw::call(g_methods.layer_mirror, scene_layer) != 0)
    {
        return;
    }

    if (g_main_scene.exchange(scene_layer) != scene_layer) {
        log_info("Main scene layer {:x}", (uintptr_t)scene_layer);
    }
}

void on_prepare_output_draw(void* prepare_output_layer) {
    const auto scene = (void*)refw::call(g_methods.layer_parent, prepare_output_layer);

    if (scene == nullptr || scene != g_main_scene.load()) {
        return;
    }

    pipeline_probe::on_prepare_output_draw(scene);

    const auto depth_texture = refw::reflection_pointer(scene, "DepthStencilTex");
    const auto motion_target = refw::reflection_pointer(scene, "VelocityTarget");
    const auto color_target = scene_color_target(prepare_output_layer);

    Inputs inputs{};
    inputs.scene_layer = scene;
    inputs.depth = render_objects::d3d12_resource(depth_texture);
    inputs.motion = render_objects::d3d12_resource(motion_target);
    inputs.color = render_objects::d3d12_resource(color_target);

    std::scoped_lock _{g_mutex};
    inputs.frame = g_inputs.frame + 1;
    g_inputs = inputs;
}

void* main_scene() {
    return g_main_scene.load();
}

Inputs latest() {
    std::scoped_lock _{g_mutex};
    return g_inputs;
}

std::string describe() {
    const auto in = latest();
    return std::format("scene {:x} (frame {}): depth {}, motion {}, color {}", (uintptr_t)in.scene_layer, in.frame,
        describe_resource(in.depth), describe_resource(in.motion), describe_resource(in.color));
}
} // namespace scene_inputs
