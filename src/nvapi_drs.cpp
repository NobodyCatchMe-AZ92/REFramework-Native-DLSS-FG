#include <windows.h>

#include <memory>

#include "log.hpp"

#include "nvapi_drs.hpp"

namespace {
// NVAPI's public interface ids (nvapi_interface.h) and structures (nvapi.h), declared here to avoid the SDK.
constexpr uint32_t ID_INITIALIZE = 0x0150E828;
constexpr uint32_t ID_DRS_CREATE_SESSION = 0x0694D52E;
constexpr uint32_t ID_DRS_DESTROY_SESSION = 0xDAD9CFF8;
constexpr uint32_t ID_DRS_LOAD_SETTINGS = 0x375DBD6B;
constexpr uint32_t ID_DRS_FIND_APPLICATION_BY_NAME = 0xEEE566B2;
constexpr uint32_t ID_DRS_GET_CURRENT_GLOBAL_PROFILE = 0x617BFF9F;
constexpr uint32_t ID_DRS_ENUM_SETTINGS = 0xAE3039DA;
constexpr uint32_t ID_DRS_GET_SETTING_NAME_FROM_ID = 0xD61CBE6E;

constexpr size_t UNICODE_STRING = 2048; // NvAPI_UnicodeString: NvU16[2048]

struct BinarySetting {
    uint32_t length;
    uint8_t data[4096];
};

struct DrsSetting { // NVDRS_SETTING_V1
    uint32_t version;
    uint16_t name[UNICODE_STRING];
    uint32_t id;
    uint32_t type; // 0 DWORD, 1 binary, 2 string, 3 wstring
    uint32_t location;
    uint32_t is_current_predefined;
    uint32_t is_predefined_valid;
    union {
        uint32_t u32;
        BinarySetting binary;
        uint16_t wstr[UNICODE_STRING];
    } predefined;
    union {
        uint32_t u32;
        BinarySetting binary;
        uint16_t wstr[UNICODE_STRING];
    } current;
};

struct DrsApplication { // NVDRS_APPLICATION_V1
    uint32_t version;
    uint32_t is_predefined;
    uint16_t app_name[UNICODE_STRING];
    uint16_t friendly_name[UNICODE_STRING];
    uint16_t launcher[UNICODE_STRING];
};

struct DrsProfile { // NVDRS_PROFILE_V1
    uint32_t version;
    uint16_t name[UNICODE_STRING];
    uint32_t gpu_support;
    uint32_t is_predefined;
    uint32_t num_applications;
    uint32_t num_settings;
};

constexpr uint32_t make_version(size_t size) {
    return (uint32_t)size | (1u << 16);
}

using QueryInterface = void* (*)(uint32_t id);

template <typename Fn>
Fn get(QueryInterface query, uint32_t id) {
    return (Fn)query(id);
}

std::string narrow(const uint16_t* text) {
    std::string out{};

    for (size_t i = 0; i < UNICODE_STRING && text[i] != 0; ++i) {
        out += text[i] < 128 ? (char)text[i] : '?';
    }

    return out;
}
} // namespace

namespace nvapi_drs {
std::vector<Setting> read_settings(std::string* profile_name, bool global_profile) {
    std::vector<Setting> out{};
    const auto module = GetModuleHandleW(L"nvapi64.dll");
    const auto query = module != nullptr ? (QueryInterface)GetProcAddress(module, "nvapi_QueryInterface") : nullptr;

    if (query == nullptr) {
        return out;
    }

    using Initialize = int (*)();
    using CreateSession = int (*)(void** session);
    using DestroySession = int (*)(void* session);
    using LoadSettings = int (*)(void* session);
    using FindApplication = int (*)(void* session, const uint16_t* name, void** profile, DrsApplication* application);
    using GetGlobalProfile = int (*)(void* session, void** profile);
    using EnumSettings = int (*)(void* session, void* profile, uint32_t start, uint32_t* count, DrsSetting* settings);
    using GetSettingName = int (*)(uint32_t id, uint16_t* name);

    const auto initialize = get<Initialize>(query, ID_INITIALIZE);
    const auto create_session = get<CreateSession>(query, ID_DRS_CREATE_SESSION);
    const auto destroy_session = get<DestroySession>(query, ID_DRS_DESTROY_SESSION);
    const auto load_settings = get<LoadSettings>(query, ID_DRS_LOAD_SETTINGS);
    const auto find_application = get<FindApplication>(query, ID_DRS_FIND_APPLICATION_BY_NAME);
    const auto get_global_profile = get<GetGlobalProfile>(query, ID_DRS_GET_CURRENT_GLOBAL_PROFILE);
    const auto enum_settings = get<EnumSettings>(query, ID_DRS_ENUM_SETTINGS);
    const auto get_setting_name = get<GetSettingName>(query, ID_DRS_GET_SETTING_NAME_FROM_ID);

    if (initialize == nullptr || create_session == nullptr || destroy_session == nullptr || load_settings == nullptr || find_application == nullptr ||
        get_global_profile == nullptr || enum_settings == nullptr || initialize() != 0)
    {
        return out;
    }

    void* session{};

    if (create_session(&session) != 0) {
        return out;
    }

    if (load_settings(session) == 0) {
        // This executable's profile, else the global one.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring name{exe};
        name = name.substr(name.find_last_of(L"\\/") + 1);

        uint16_t app_name[UNICODE_STRING]{};

        for (size_t i = 0; i < name.size() && i + 1 < UNICODE_STRING; ++i) {
            app_name[i] = (uint16_t)name[i];
        }

        void* profile{};
        auto application = std::make_unique<DrsApplication>();
        application->version = make_version(sizeof(DrsApplication));

        if (global_profile || find_application(session, app_name, &profile, application.get()) != 0 || profile == nullptr) {
            profile = nullptr;
            get_global_profile(session, &profile);
        }

        if (profile_name != nullptr) {
            *profile_name = profile == nullptr ? "none" : (application->app_name[0] != 0 ? "application profile" : "global profile");
        }

        // Settings in batches.
        constexpr uint32_t BATCH = 64;
        auto settings = std::make_unique<DrsSetting[]>(BATCH);

        for (uint32_t start = 0; profile != nullptr; start += BATCH) {
            for (uint32_t i = 0; i < BATCH; ++i) {
                settings[i].version = make_version(sizeof(DrsSetting));
            }

            uint32_t count = BATCH;

            if (enum_settings(session, profile, start, &count, settings.get()) != 0 || count == 0) {
                break;
            }

            for (uint32_t i = 0; i < count; ++i) {
                const auto& s = settings[i];
                Setting setting{};
                setting.id = s.id;
                setting.location = s.location;
                setting.is_dword = s.type == 0;
                setting.value = s.type == 0 ? s.current.u32 : 0;
                setting.name = narrow(s.name);

                if (setting.name.empty() && get_setting_name != nullptr) {
                    uint16_t found[UNICODE_STRING]{};

                    if (get_setting_name(s.id, found) == 0) {
                        setting.name = narrow(found);
                    }
                }

                out.push_back(setting);
            }

            if (count < BATCH) {
                break;
            }
        }
    }

    destroy_session(session);
    return out;
}
} // namespace nvapi_drs
