#include <windows.h>

#include <atomic>
#include <bit>
#include <format>
#include <string>

#include "log.hpp"
#include "refw.hpp"
#include "render_objects.hpp"

#include "pipeline_probe.hpp"

namespace {
int g_percent{};
std::atomic<int> g_dump_in_frames{-1}; // frames until the next dump, -1 = none

// The invoke wrapper passes and returns floats as doubles.
float as_float(uint64_t raw) {
    return (float)std::bit_cast<double>(raw);
}

void* render_config() {
    static const auto get_render_config = refw::find_method("via.render.Renderer", "get_RenderConfig");
    const auto renderer = refw::native_singleton("via.render.Renderer");
    return renderer != nullptr ? (void*)refw::call(get_render_config, renderer) : nullptr;
}

// A layer's children: a native array {elements, count} inside the layer, found as the first array whose first element
// says this layer is its parent.
size_t g_children_offset{};

std::pair<void* const*, uint32_t> children(void* layer) {
    static const auto get_parent = refw::find_method("via.render.RenderLayer", "get_Parent");
    const auto base = (const uint8_t*)layer;

    if (g_children_offset == 0) {
        for (size_t o = sizeof(void*); o < 0x200; o += sizeof(void*)) {
            const auto elements = *(void* const* const*)(base + o);
            const auto count = *(const uint32_t*)(base + o + sizeof(void*));

            if (elements == nullptr || IsBadReadPtr(elements, sizeof(void*)) || count == 0 || count > 256) {
                continue;
            }

            const auto first = elements[0];

            if (refw::is_managed_object(first) && (void*)refw::call(get_parent, first) == layer) {
                g_children_offset = o;
                log_info("probe: render layer children at +{:x}", o);
                break;
            }
        }

        if (g_children_offset == 0) {
            return {nullptr, 0};
        }
    }

    return {*(void* const* const*)(base + g_children_offset), *(const uint32_t*)(base + g_children_offset + sizeof(void*))};
}

std::string describe(ID3D12Resource* resource) {
    if (resource == nullptr) {
        return "-";
    }

    const auto desc = resource->GetDesc();
    return std::format("{}x{} fmt {} @{:x}", desc.Width, desc.Height, (int)desc.Format, (uintptr_t)resource);
}

void dump_layer(void* layer, int depth) {
    if (layer == nullptr || depth > 6 || IsBadReadPtr(layer, 0x300)) {
        return;
    }

    const auto indent = std::string((size_t)depth * 2, ' ');
    log_info("probe: {}{} @ {:x}", indent, refw::type_name(layer), (uintptr_t)layer);

    // Render targets and textures the layer holds, with the size of what they resolve to.
    for (size_t o = sizeof(void*); o < 0x300; o += sizeof(void*)) {
        const auto member = *(void* const*)((const uint8_t*)layer + o);
        const auto type = render_objects::type_name(member);

        if (type == "via.render.TargetState" || type == "via.render.Texture" || type == "via.render.ShaderResourceView") {
            log_info("probe: {}  +{:x} {} {}", indent, o, type.substr(11), describe(render_objects::d3d12_resource(member)));
        }
    }

    const auto [elements, count] = children(layer);

    for (uint32_t i = 0; i < count; ++i) {
        dump_layer(elements[i], depth + 1);
    }
}
} // namespace

namespace pipeline_probe {
void configure(int image_quality_percent) {
    // Set live while the game runs (after the startup dumps): one more dump of the scene as it is now.
    if (image_quality_percent > 0 && image_quality_percent != g_percent && g_dump_in_frames.load() < 0) {
        g_dump_in_frames = 1;
    }

    g_percent = image_quality_percent;
}

void on_begin_rendering() {
    if (g_percent <= 0) {
        return;
    }

    static const auto get_rate = refw::find_method("via.render.RenderConfig", "get_ImageQualityRate");
    static const auto set_rate = refw::find_method("via.render.RenderConfig", "set_ImageQualityRate");
    static const auto get_aa = refw::find_method("via.render.RenderConfig", "get_AntiAliasing");
    static int frames = 0;

    // Wait for the game to settle (it applies its own settings while loading): dump at its own rate, then set ours and
    // dump again once it has taken effect.
    if (++frames != 600 && frames != 720) {
        return;
    }

    const auto config = render_config();

    if (config == nullptr || get_rate == nullptr || set_rate == nullptr) {
        log_error("probe: no render config or ImageQualityRate methods");
        return;
    }

    log_info("probe: ImageQualityRate {:.2f}, AntiAliasing {}", as_float(refw::call(get_rate, config)),
        (int)refw::call(get_aa, config));

    if (frames == 600) {
        g_dump_in_frames = 1;
        return;
    }

    const float rate = (float)g_percent / 100.0f;
    uint64_t arg = std::bit_cast<uint64_t>((double)rate);
    refw::call_args(set_rate, config, &arg, 1);
    log_info("probe: ImageQualityRate set to {:.2f}, now {:.2f}", rate, as_float(refw::call(get_rate, config)));
    g_dump_in_frames = 120;
}

void on_prepare_output_draw(void* scene_layer) {
    static int dumps = 0;
    const auto n = g_dump_in_frames.load();

    if (n < 0) {
        return;
    }

    if (n > 0) {
        g_dump_in_frames = n - 1;
        return;
    }

    log_info("probe: ---- main scene layer tree (dump {}) ----", ++dumps);

    // Named targets of the scene layer (reflection properties), to tell which member is which.
    for (const auto name : {"MainTarget", "HDRTarget", "HDRMainTarget", "SceneTarget", "ColorTarget", "LDRTarget", "PostMainTarget",
             "PreMainTarget", "VelocityTarget", "LightingTarget", "TemporalAATarget", "PrevMainTarget", "HistoryTarget", "MainTex",
             "HDRTex", "SceneTex", "DepthStencilTex", "DepthTarget", "LinearDepthTarget", "TransparentTarget", "PostEffectTarget",
             "GBufferTarget", "SceneInfo", "JitterDisableSceneInfo", "JitterDisablePostSceneInfo", "FilterSceneInfo", "ZPrepassSceneInfo",
             "DepthDistortionSceneInfo"})
    {
        const auto value = refw::reflection_pointer(scene_layer, name);

        if (value != nullptr) {
            const auto type = render_objects::type_name(value);
            log_info("probe: name {} = {:x} {} {}", name, (uintptr_t)value, type, type.empty() ? "" : describe(render_objects::d3d12_resource(value)));
        }
    }

    dump_layer(scene_layer, 0);
    log_info("probe: ---- end ----");
    g_dump_in_frames = -1;
}
} // namespace pipeline_probe

namespace pipeline_probe {
void find_methods(const char* const* words, size_t count) {
    const auto sdk = refw::sdk();
    const auto tdb = sdk->functions->get_tdb();
    const auto total = sdk->tdb->get_num_methods(tdb);
    int found = 0;

    for (unsigned int i = 0; i < total && found < 1000; ++i) {
        const auto method = sdk->tdb->get_method(tdb, i);
        const auto name = method != nullptr ? sdk->method->get_name(method) : nullptr;

        if (name == nullptr) {
            continue;
        }

        const std::string_view n{name};
        bool match = false;

        for (size_t w = 0; w < count && !match; ++w) {
            match = n.find(words[w]) != std::string_view::npos;
        }

        if (!match) {
            continue;
        }

        const auto type = sdk->method->get_declaring_type(method);
        char type_name[256]{};
        unsigned int len = 0;

        if (type == nullptr || sdk->type_definition->get_full_name(type, type_name, sizeof(type_name), &len) != REFRAMEWORK_ERROR_NONE ||
            !std::string_view{type_name}.starts_with("via."))
        {
            continue;
        }

        log_info("probe: method {}.{} ({} params)", type_name, name, sdk->method->get_num_params(method));
        ++found;
    }

    log_info("probe: {} matching methods of {}", found, total);
}
} // namespace pipeline_probe

namespace pipeline_probe {
void dump_code(const char* type, const char* method_name) {
    const auto fn = (const uint8_t*)refw::method_function(refw::find_method(type, method_name));

    if (fn == nullptr) {
        log_info("probe: {}.{} not found", type, method_name);
        return;
    }

    auto hex = [](const uint8_t* p, size_t n) {
        std::string s{};

        for (size_t i = 0; i < n; ++i) {
            s += std::format("{:02x} ", p[i]);
        }

        return s;
    };

    log_info("probe: {}.{} @ {:x}: {}", type, method_name, (uintptr_t)fn, hex(fn, 64));

    // Follow the first call or jump (the wrapper calling the engine function).
    for (size_t i = 0; i < 60; ++i) {
        if (fn[i] == 0xE8 || fn[i] == 0xE9) {
            const auto target = fn + i + 5 + *(const int32_t*)(fn + i + 1);
            log_info("probe:   +{:x} {} -> {:x}: {}", i, fn[i] == 0xE8 ? "call" : "jmp", (uintptr_t)target, hex(target, 160));
            break;
        }
    }
}
} // namespace pipeline_probe
