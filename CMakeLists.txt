cmake_minimum_required(VERSION 3.24)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GRML_VERSION LIMIT_COUNT 1)
string(STRIP "${GRML_VERSION}" GRML_VERSION)

project(GloriousRepublicModLoader VERSION ${GRML_VERSION} LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "GloriousRepublicModLoader requires a 64-bit Windows (x64) toolchain.")
endif()

if(MSVC)
    string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT) across all targets so players never need VCRedist DLLs
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cmake_policy(SET CMP0169 OLD)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(EmbedGrmlOffsets)
include(EmbedUiAssets)
include(FetchContent)

# Optional sibling-repo cached _deps fallback for offline builds
set(_sibling_deps "${CMAKE_CURRENT_SOURCE_DIR}/../WorkersAndResources-Modding/build/_deps")
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_MINHOOK AND EXISTS "${_sibling_deps}/minhook-src/include/MinHook.h")
    set(FETCHCONTENT_SOURCE_DIR_MINHOOK "${_sibling_deps}/minhook-src" CACHE PATH "" FORCE)
endif()
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_IMGUI AND EXISTS "${_sibling_deps}/imgui-src/imgui.h")
    set(FETCHCONTENT_SOURCE_DIR_IMGUI "${_sibling_deps}/imgui-src" CACHE PATH "" FORCE)
endif()

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(imgui)
FetchContent_Populate(minhook)
FetchContent_GetProperties(minhook SOURCE_DIR minhook_SOURCE_DIR)

# ── Shared MinHook static library ─────────────────────────────────────────────
add_library(grml_minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(grml_minhook PUBLIC ${minhook_SOURCE_DIR}/include)
set_target_properties(grml_minhook PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ── Shared Dear ImGui static library (DX11 + Win32 + Docking) ──────────────────
add_library(grml_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(grml_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(grml_imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
set_target_properties(grml_imgui PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ── Shared GRML common library (PE scanner, INI, JSON, Manifest/TopoSort) ─────
add_library(grml_common STATIC
    src/common/ini_util.cpp
    src/common/json_min.cpp
    src/common/manifest.cpp
    src/common/offset_resolver.cpp
    src/common/pe_scanner.cpp
    src/common/ui_schema.cpp
    src/common/zip_reader.cpp
    src/common/vfs_index.cpp
)
target_include_directories(grml_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(grml_common PUBLIC bcrypt version)
target_compile_definitions(grml_common PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    GRML_BUILD_VERSION="${GRML_VERSION}"
)
set_target_properties(grml_common PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
if(MSVC)
    target_compile_options(grml_common PRIVATE /W4 /WX- /MP /wd4100 /wd4201)
endif()

# ── grml_runtime.dll (Native Injected Host & F9 Mod Settings Hub) ─────────────
add_library(grml_runtime SHARED
    src/runtime/dllmain.cpp
    src/runtime/ring_logger.cpp
    src/runtime/save_events.cpp
    src/runtime/vfs_redirect.cpp
    src/runtime/hook_manager.cpp
    src/runtime/overlay_hub.cpp
    src/runtime/tesmio_bridge.cpp
    src/runtime/plugin_supervisor.cpp
)
grml_embed_offsets(grml_runtime "${CMAKE_CURRENT_SOURCE_DIR}/config/grml_offsets.ini"
    HEADER_NAME "embedded_grml_offsets.hpp"
    SYMBOL_NAME "kEmbeddedGrmlOffsetsIni"
)
target_include_directories(grml_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(grml_runtime PRIVATE
    grml_common
    grml_minhook
    grml_imgui
    d3d11
    dxgi
    d3dcompiler
)
set_target_properties(grml_runtime PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
)
if(MSVC)
    target_compile_options(grml_runtime PRIVATE /W4 /WX- /MP /wd4100 /wd4201 /wd4324)
    target_link_options(grml_runtime PRIVATE /DEBUG:FULL)
endif()

# ── WebView2 SDK (cached in .cache/webview2 or fetched via FetchContent) ───────
set(_wv2_cache "${CMAKE_CURRENT_SOURCE_DIR}/.cache/webview2")
if(EXISTS "${_wv2_cache}/build/native/include/WebView2.h")
    set(WEBVIEW2_ROOT "${_wv2_cache}")
else()
    FetchContent_Declare(
        webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
    )
    FetchContent_Populate(webview2)
    FetchContent_GetProperties(webview2 SOURCE_DIR WEBVIEW2_ROOT)
endif()

# ── GloriousRepublicModLoader.exe (Native Win32 + WebView2 + Injector CLI) ─────
add_executable(grml_injector WIN32
    src/launcher/main.cpp
    src/launcher/injector.cpp
    src/launcher/http_server.cpp
    src/launcher/mcp_server.cpp
    src/launcher/webview_host.cpp
    src/launcher/launcher.rc
)
grml_embed_ui_assets(grml_injector "${CMAKE_CURRENT_SOURCE_DIR}/build/dist")
grml_embed_offsets(grml_injector "${CMAKE_CURRENT_SOURCE_DIR}/config/grml_offsets.ini"
    HEADER_NAME "embedded_grml_offsets.hpp"
    SYMBOL_NAME "kEmbeddedGrmlOffsetsIni"
)
grml_embed_offsets(grml_injector "${CMAKE_CURRENT_SOURCE_DIR}/config/grml.ini"
    HEADER_NAME "embedded_grml_ini.hpp"
    SYMBOL_NAME "kEmbeddedDefaultGrmlIni"
)
target_include_directories(grml_injector PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WEBVIEW2_ROOT}/build/native/include"
)
target_link_libraries(grml_injector PRIVATE
    grml_common
    "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib"
    ws2_32
    dwmapi
    comdlg32
    ole32
    shell32
    shlwapi
    version
    wevtapi
    winhttp
)
set_target_properties(grml_injector PROPERTIES
    OUTPUT_NAME "GloriousRepublicModLoader"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
)
if(MSVC)
    target_compile_options(grml_injector PRIVATE /W4 /WX- /MP /wd4100 /wd4201)
    target_link_options(grml_injector PRIVATE /DEBUG:FULL /ENTRY:wWinMainCRTStartup)
endif()

# ── Stage runtime offsets next to binaries ─────────────────────────────────────
add_custom_command(TARGET grml_runtime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/grml_offsets.ini"
        "${CMAKE_BINARY_DIR}/bin/grml_offsets.ini"
)

# ── Pure-C ABI compile check (C11 compiler validation of include/grml_api.h) ──
add_library(grml_api_c_check OBJECT src/common/grml_api_c_check.c)
set_target_properties(grml_api_c_check PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_include_directories(grml_api_c_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
