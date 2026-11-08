// Native DLSS + Frame Generation (refw_framegen.dll): DLSS super resolution and frame generation for RE Engine games, as a
// REFramework plugin.
// Uses only REFramework's official plugin interface (include/reframework/API.h).

#include <windows.h>

#include <reframework/API.h>

#include "crash_log.hpp"
#include "framegen.hpp"

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
    version->game_name = nullptr; // any RE Engine game: the plugin checks what it needs itself
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    return framegen::initialize(param);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        framegen::early_init(module);
        crash_log::install();
    }

    return TRUE;
}
