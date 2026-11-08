cmake_minimum_required(VERSION 3.20)
project(refw_framegen C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT: one self-contained DLL

add_library(refw_framegen SHARED
    src/plugin.cpp
    src/framegen.cpp
    src/hooks.cpp
    src/log.cpp
    src/unhook_guard.cpp
    src/crash_log.cpp
    src/refw.cpp
    src/engine_hooks.cpp
    src/render_objects.cpp
    src/scene_inputs.cpp
    src/pipeline_probe.cpp
    src/dlss_pass.cpp
    src/cl_state.cpp
    src/bloom_composite.cpp
    src/upscale_guard.cpp
    src/wait_probe.cpp
    src/frame_pacer.cpp
    src/scale_blit.cpp
    src/own_dof.cpp
    src/motion_blur.cpp
    src/own_bloom.cpp
    src/re2_dither.cpp
    src/cas_sharpen.cpp
    src/bloom_transfer.cpp
    src/descriptor_map.cpp
    src/examine_stabilizer.cpp
    src/bloom_stabilizer.cpp
    src/gpu_timer.cpp
    src/game_profile.cpp
    src/hud_split.cpp
    src/nvapi_drs.cpp
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)

target_include_directories(refw_framegen PRIVATE
    src
    third_party/reframework/include
    third_party/streamline/include
    third_party/minhook/include
)

target_compile_definitions(refw_framegen PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(refw_framegen PRIVATE /W3 /utf-8 /EHsc)
target_link_libraries(refw_framegen PRIVATE d3d12 dxgi dxguid wintrust crypt32)

# Symbols for Release too (the .pdb stays here; it maps crash offsets in the log to source lines).
target_compile_options(refw_framegen PRIVATE /Zi)
# /PDBALTPATH: only the symbol file's name goes into the DLL, not the build folder's path (written out: CMake escaped
# %_PDB% into the literal name %%%refw_framegen.pdb%%%, which debuggers then could not find).
target_link_options(refw_framegen PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP /PDBALTPATH:refw_framegen.pdb)
