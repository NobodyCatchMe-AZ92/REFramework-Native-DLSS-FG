#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

// Diagnostics (setting probe_descriptors, at startup): which texture each descriptor points to. Hooks the device's
// CreateShaderResourceView / CreateUnorderedAccessView / CopyDescriptors(Simple) and keeps a map from CPU descriptor
// handles to the resource (and view format). Used to list the textures a compute pass reads (RE4's upscale pass: where
// its depth of field and bloom come from). Costs CPU time: off by default.
namespace descriptor_map {
bool install(ID3D12Device* device);
bool installed();

// The texture behind a CPU descriptor handle, described ("2560x1440 fmt 26 SRV 'name'"), or "" if unknown.
std::string describe(SIZE_T cpu_handle);
} // namespace descriptor_map
