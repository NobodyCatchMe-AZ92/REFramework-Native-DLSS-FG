#pragma once

#include <cstdint>
#include <string>

#include <reframework/API.h>

// Thin helpers over REFramework's official plugin API (the game's type database, singletons, objects).
namespace refw {
void init(const REFrameworkPluginInitializeParam* param);
const REFrameworkPluginInitializeParam* param();
const REFrameworkSDKData* sdk();

// The type database's own version number (from its header: "TDB\0" + version); 0 if unreadable.
uint32_t tdb_version();

REFrameworkTypeDefinitionHandle find_type(const char* name);
REFrameworkMethodHandle find_method(const char* type, const char* name);
void* method_function(REFrameworkMethodHandle method); // the method's code (for native methods: the engine wrapper)
void* native_singleton(const char* type);

// A fresh instance of a native type (e.g. a render layer), for reading its vtable. Never freed; call once per type.
void* create_native_instance(const char* type);

bool is_managed_object(const void* object);
std::string type_name(const void* managed_object); // full name, "" if not a managed object
bool is_a(const void* managed_object, const char* type_name);

// Calls a method (no arguments) and returns its result as 8 raw bytes (pointer, bool, int...). 0 if it threw or failed.
uint64_t call(REFrameworkMethodHandle method, void* thisptr);

// The same with arguments, one 8-byte slot each (float arguments and results go through the invoke wrapper as doubles).
uint64_t call_args(REFrameworkMethodHandle method, void* thisptr, uint64_t* args, uint32_t count);

// A pointer-sized reflection property of a managed object (render layers keep their targets in these), or nullptr.
void* reflection_pointer(void* managed_object, const char* name);
} // namespace refw
