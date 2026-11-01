cmake_minimum_required(VERSION 3.21)
project(DarkSouls3VR2 VERSION 2.0.0 LANGUAGES C CXX ASM_MASM)
# DS3VR 2.0 - Dark Souls III VR with the game's own stereo (no geo-11). Framework forked from batman_origins (core/), game layer
# in src/game/ds3. Configure:  cmake -S ds3vr2 -B ds3vr2/build -G "Visual Studio 17 2022" -A x64
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "DS3VR2 requires MSVC x64: -G \"Visual Studio 17 2022\" -A x64")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Dynamic CRT (/MD): NVIDIA's NGX static library is built for it (AKVR / AKN use the same).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

set(REFS "${CMAKE_CURRENT_SOURCE_DIR}/../references")

# --- MinHook (BSD-2) from the reference clone ---
add_subdirectory("${REFS}/minhook" "${CMAKE_BINARY_DIR}/_deps/minhook")

# --- OpenXR loader (Apache-2.0), static, from the reference clone ---
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REFS}/OpenXR-SDK-Source" "${CMAKE_BINARY_DIR}/_deps/openxr")

# --- Dear ImGui 1.92.8 (MIT), vendored: the same version the Batman panel code is written against ---
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)

# --- the mod: dinput8.dll ---
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(ds3vr2 SHARED ${CORE_SOURCES}
    src/game/adapter_registry.cpp
    src/game/ds3/bootstrap.cpp
    src/game/ds3/vrmode.cpp
    src/game/ds3/proxy.cpp
    src/game/ds3/settings.cpp
    src/game/ds3/adapter.cpp
    src/game/ds3/camera.cpp
    src/game/ds3/camera_stub.asm
    src/game/ds3/lightgrid.cpp
    src/game/ds3/tilefix.cpp
    src/game/ds3/afw.cpp
    src/game/ds3/focus.cpp
    src/game/ds3/lightgrid_stub.asm
    src/game/ds3/stereo.cpp
    src/game/ds3/dlss.cpp
    src/game/ds3/framerate.cpp
    src/game/ds3/picsplit.cpp
    src/game/ds3/hwwatch.cpp
    src/game/ds3/shaderid.cpp
    src/game/ds3/envtime.cpp
    src/game/ds3/eyesize.cpp
    src/game/ds3/markers.cpp
    src/game/ds3/shadowres.cpp
    src/game/ds3/contact.cpp
    src/game/ds3/offline.cpp
    src/game/ds3/sunrays.cpp
    src/game/ds3/offaxis.cpp
    src/game/ds3/loadscreen.cpp
    src/game/ds3/dinput8.def)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${CMAKE_BINARY_DIR}/generated/bvr_version.h"
 "#pragma once\n#define BVR_VERSION \"${PROJECT_VERSION}\"\n#define BVR_BUILD_ID \"DS3VR2-M0\"\n")
target_include_directories(ds3vr2 PRIVATE src "${CMAKE_BINARY_DIR}/generated" "${REFS}/minhook/include" third_party/ngx/include)
target_compile_definitions(ds3vr2 PRIVATE BVR_WITH_OPENXR)
target_link_libraries(ds3vr2 PRIVATE minhook imgui openxr_loader
    d3d11 dxgi d3dcompiler dbghelp windowscodecs ole32 bcrypt shlwapi ws2_32
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/lib/nvsdk_ngx_d.lib")
set_target_properties(ds3vr2 PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")

# Crash dumps name the exact function (V12h, 2026-10-31): a PDB for the release build, same optimisation.
target_compile_options(ds3vr2 PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(ds3vr2 PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
