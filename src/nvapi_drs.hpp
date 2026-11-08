#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Read-only access to NVIDIA's driver profiles (NVAPI DRS, through the nvapi64.dll the game loaded): the per-game and
// global settings the NVIDIA app / control panel writes, e.g. a forced DLSS preset.
namespace nvapi_drs {
struct Setting {
    uint32_t id{};
    std::string name;      // the driver's name for it ("" if it has none)
    uint32_t value{};      // the current value (DWORD settings)
    uint32_t location{};   // 0 this profile, 1 global, 2 base, 3 default
    bool is_dword{};
};

// The settings that apply to this executable (its application profile), or the global profile if it has none.
// Empty if NVAPI is unavailable. Slow-ish (milliseconds): not every frame.
std::vector<Setting> read_settings(std::string* profile_name = nullptr, bool global_profile = false);
} // namespace nvapi_drs
