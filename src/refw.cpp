#include <windows.h>

#include "log.hpp"
#include "refw.hpp"

namespace {
const REFrameworkPluginInitializeParam* g_param{};
} // namespace

namespace refw {
void init(const REFrameworkPluginInitializeParam* param) {
    g_param = param;
}

const REFrameworkPluginInitializeParam* param() {
    return g_param;
}

const REFrameworkSDKData* sdk() {
    return g_param != nullptr ? g_param->sdk : nullptr;
}

uint32_t tdb_version() {
    const auto tdb = (const uint8_t*)sdk()->functions->get_tdb();

    if (tdb == nullptr || IsBadReadPtr(tdb, 8) || tdb[0] != 'T' || tdb[1] != 'D' || tdb[2] != 'B') {
        return 0;
    }

    return *(const uint32_t*)(tdb + 4);
}

REFrameworkTypeDefinitionHandle find_type(const char* name) {
    return sdk()->tdb->find_type(sdk()->functions->get_tdb(), name);
}

REFrameworkMethodHandle find_method(const char* type, const char* name) {
    return sdk()->tdb->find_method(sdk()->functions->get_tdb(), type, name);
}

void* method_function(REFrameworkMethodHandle method) {
    return method != nullptr ? sdk()->method->get_function(method) : nullptr;
}

void* native_singleton(const char* type) {
    return sdk()->functions->get_native_singleton(type);
}

void* create_native_instance(const char* type) {
    const auto t = find_type(type);
    const auto info = t != nullptr ? sdk()->type_definition->get_type_info(t) : nullptr;
    return info != nullptr ? sdk()->type_info->create_instance(info) : nullptr;
}

bool is_managed_object(const void* object) {
    return object != nullptr && !IsBadReadPtr(object, sizeof(void*)) && sdk()->managed_object->is_managed_object((void*)object);
}

std::string type_name(const void* managed_object) {
    if (!is_managed_object(managed_object)) {
        return "";
    }

    const auto t = sdk()->managed_object->get_type_definition((REFrameworkManagedObjectHandle)managed_object);
    char buffer[256]{};
    unsigned int len = 0;

    if (t == nullptr || sdk()->type_definition->get_full_name(t, buffer, sizeof(buffer), &len) != REFRAMEWORK_ERROR_NONE) {
        return "";
    }

    return std::string{buffer, len};
}

bool is_a(const void* managed_object, const char* name) {
    if (!is_managed_object(managed_object)) {
        return false;
    }

    const auto t = sdk()->managed_object->get_type_definition((REFrameworkManagedObjectHandle)managed_object);
    return t != nullptr && sdk()->type_definition->is_derived_from_by_name(t, name);
}
} // namespace refw

namespace refw {
uint64_t call(REFrameworkMethodHandle method, void* thisptr) {
    if (method == nullptr) {
        return 0;
    }

    // InvokeRet in API.h: 128 bytes of result, then the "exception thrown" flag.
    struct {
        uint8_t bytes[128];
        bool exception_thrown;
    } ret{};

    if (sdk()->method->invoke(method, thisptr, nullptr, 0, &ret, sizeof(ret)) != REFRAMEWORK_ERROR_NONE || ret.exception_thrown) {
        return 0;
    }

    return *(uint64_t*)ret.bytes;
}

void* reflection_pointer(void* managed_object, const char* name) {
    // Render layers are native objects with reflection data: REFramework's managed object check can reject them, so
    // only check that the object is readable.
    if (managed_object == nullptr || IsBadReadPtr(managed_object, sizeof(void*))) {
        return nullptr;
    }

    const auto desc = sdk()->managed_object->get_reflection_property_descriptor((REFrameworkManagedObjectHandle)managed_object, name);
    const auto getter = desc != nullptr ? sdk()->reflection_property->get_getter(desc) : nullptr;
    if (getter == nullptr) {
        return nullptr;
    }

    // The reported size is not the value's size for these (render layers report 112 or 160 for a pointer), so the getter
    // writes into a buffer big enough for any value and the pointer is its first 8 bytes.
    alignas(16) uint8_t value[0x400]{};
    getter(desc, (REFrameworkManagedObjectHandle)managed_object, value);
    return *(void**)value;
}
} // namespace refw

namespace refw {
uint64_t call_args(REFrameworkMethodHandle method, void* thisptr, uint64_t* args, uint32_t count) {
    if (method == nullptr) {
        return 0;
    }

    struct {
        uint8_t bytes[128];
        bool exception_thrown;
    } ret{};

    if (sdk()->method->invoke(method, thisptr, (void**)args, count * sizeof(void*), &ret, sizeof(ret)) != REFRAMEWORK_ERROR_NONE ||
        ret.exception_thrown)
    {
        return 0;
    }

    return *(uint64_t*)ret.bytes;
}
} // namespace refw
