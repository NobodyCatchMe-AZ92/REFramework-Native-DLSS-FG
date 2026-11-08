#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Minimal hook helpers. Both only touch what they are given (a pointer slot, or one object's vtable pointer), so they
// chain with other hooks (REFramework, ReShade, OptiScaler, Streamline) instead of fighting them.

// Replaces one function pointer (an import table slot or a vtable entry), keeping the previous value.
class PointerHook {
public:
    PointerHook(void** slot, void* replacement);
    ~PointerHook();

    template <typename T>
    T original() const { return (T)m_original; }

private:
    void** m_slot{};
    void* m_original{};
};

// Gives one object its own copy of its vtable with some entries replaced; other objects of the same class are not
// affected. Never restored (the object may be destroyed first), so instances live for the whole process.
class InstanceVtableHook {
public:
    InstanceVtableHook(void* object, size_t entries);

    void hook(size_t index, void* replacement);

    template <typename T>
    T original(size_t index) const { return (T)m_original_vtable[index]; }

    void* object() const { return m_object; }

private:
    void* m_object{};
    void** m_original_vtable{};
    std::vector<void*> m_vtable{};
};

// The import table slot of `function` from `dll` in a module (case-insensitive DLL name), or nullptr.
void** find_import_slot(HMODULE module, const char* dll, const char* function);
