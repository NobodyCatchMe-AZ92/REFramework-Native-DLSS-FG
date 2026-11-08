#pragma once

#include <d3d12.h>

#include <string>

// Navigating RE Engine render objects (render layers, target states, render target views, textures) without fixed
// offsets: members are found by the engine type of what they point to (each render object reports its type through a
// virtual function), and native D3D12 objects by their vtable living in a Direct3D module. Offsets found this way are
// cached per (container type, member type), so the scanning happens once.
namespace render_objects {
// The engine type name of a render object ("via.render.Texture", ...), or "" if it doesn't look like one.
std::string type_name(const void* object);

// The first pointer member of `object` (within `range` bytes) that points to a render object of type `type`.
void* member_of_type(const void* object, const char* object_type, const char* type, size_t range = 0x200);

// Diagnostics: logs every member of `object` that is a render object or a D3D object.
void dump(const char* label, const void* object, size_t range = 0x200);

// The D3D12 resource behind a render object (Texture, or the first render target of a TargetState).
ID3D12Resource* d3d12_resource(const void* object);

// Is this pointer a native Direct3D object (its vtable is in d3d12*.dll / D3D12Core.dll)?
bool is_d3d_object(const void* object);
} // namespace render_objects
