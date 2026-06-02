cmake_minimum_required(VERSION 3.24)
project(AetherAim
    VERSION 1.0.0
    DESCRIPTION "FPS Accessibility Assistant - Adaptive mouse smoothing for gamers with motor disabilities"
    LANGUAGES CXX
)

# ============================================================================
# C++20 Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Platform Detection
# ============================================================================
if(NOT WIN32)
    message(FATAL_ERROR "AetherAim requires Windows 10/11. Current platform: ${CMAKE_SYSTEM_NAME}")
endif()

# ============================================================================
# MSVC-specific Flags
# ============================================================================
if(MSVC)
    # /O2: maximize speed; /MT: static CRT (no DLL dependency)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_compile_options(
        /W4              # Warning level 4
        /WX-             # Don't treat warnings as errors (strict but not pedantic)
        /MP              # Multi-processor compilation
        /permissive-     # Standards conformance mode
        /Zc:__cplusplus  # Correct __cplusplus macro
        /Zc:preprocessor # Conforming preprocessor
        /utf-8           # Source/execution charset UTF-8
    )

    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
    )

    # Release: full optimization for <5ms latency target
    add_compile_options(
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>   # Whole program optimization
        $<$<CONFIG:Release>:/Oi>   # Enable intrinsic functions
        $<$<CONFIG:Release>:/Ot>   # Favor fast code
    )

    add_link_options(
        $<$<CONFIG:Release>:/LTCG> # Link-time code generation
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

# ============================================================================
# FetchContent - Dependencies
# ============================================================================
include(FetchContent)

# --- Dear ImGui (DX11 backend) ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# Build Dear ImGui as a static library with Win32 + DX11 backends
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC d3d11.lib dxgi.lib d3dcompiler.lib)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_API=extern)

# --- implot (real-time plotting) ---
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG v0.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(implot)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

# --- nlohmann/json ---
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

# --- MinHook (API Hooking library for Raw Input interception) ---
FetchContent_Declare(
    minhook_src
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG 3b45d37           # Stable commit (2023-01)
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""          # HDE files are checked in directly
)
FetchContent_Populate(minhook_src)

add_library(minhook STATIC
    ${minhook_src_SOURCE_DIR}/src/buffer.c
    ${minhook_src_SOURCE_DIR}/src/hook.c
    ${minhook_src_SOURCE_DIR}/src/trampoline.c
    ${minhook_src_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_src_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC
    ${minhook_src_SOURCE_DIR}/include
    ${minhook_src_SOURCE_DIR}/src
)
set_target_properties(minhook PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)
# Mark C sources explicitly so MSVC compiles them as C
set_source_files_properties(
    ${minhook_src_SOURCE_DIR}/src/buffer.c
    ${minhook_src_SOURCE_DIR}/src/hook.c
    ${minhook_src_SOURCE_DIR}/src/trampoline.c
    ${minhook_src_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_src_SOURCE_DIR}/src/hde/hde64.c
    PROPERTIES LANGUAGE C
)

# ============================================================================
# Optional: SQLite for advanced profile storage
#
// To enable SQLite support:
//   1. Download sqlite3.c and sqlite3.h from https://www.sqlite.org/download.html
//   2. Place them in external/sqlite/
//   3. Build with: cmake -B build -DAETHER_USE_SQLITE=ON
// The SQLite backend is optional — JSON file storage works for all use cases.
// ============================================================================
option(AETHER_USE_SQLITE "Enable SQLite-based profile storage" OFF)

if(AETHER_USE_SQLITE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/external/sqlite/sqlite3.c")
        add_library(sqlite3 STATIC
            "${CMAKE_SOURCE_DIR}/external/sqlite/sqlite3.c"
        )
        target_include_directories(sqlite3 PUBLIC
            "${CMAKE_SOURCE_DIR}/external/sqlite"
        )
        target_compile_definitions(sqlite3 PRIVATE
            SQLITE_THREADSAFE=1
            SQLITE_ENABLE_FTS5
        )
        message(STATUS "SQLite: enabled (external/sqlite/sqlite3.c found)")
    else()
        message(FATAL_ERROR
            "AETHER_USE_SQLITE=ON but external/sqlite/sqlite3.c not found.\n"
            "Download the amalgamation from https://www.sqlite.org/download.html\n"
            "and place sqlite3.c + sqlite3.h in external/sqlite/"
        )
    endif()
endif()

# ============================================================================
# Project Targets
# ============================================================================
add_subdirectory(src)

# ============================================================================
# Tests (optional)
# ============================================================================
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
