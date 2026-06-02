# ============================================================================
# AetherAim Core Executable
# ============================================================================
add_executable(AetherAim
    main.cpp
    core/MouseHookManager.cpp
    core/InputProcessor.cpp
    config/ConfigManager.cpp
    config/ProfileManager.cpp
    gui/GUIApp.cpp
    gui/Widgets.cpp
    gui/CalibrationWizard.cpp
    utils/Logger.cpp
    utils/HotkeyManager.cpp
    utils/SystemTray.cpp
    injector/GameInjector.cpp
)

# Add subdirectory targets
add_subdirectory(core)
add_subdirectory(config)
add_subdirectory(gui)
add_subdirectory(utils)

# ============================================================================
# Include directories (AetherAim.exe)
# ============================================================================
target_include_directories(AetherAim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ============================================================================
# Link libraries (AetherAim.exe)
# ============================================================================
target_link_libraries(AetherAim PRIVATE
    aether_core
    aether_config
    aether_gui
    aether_utils
    imgui
    implot
    nlohmann_json::nlohmann_json
    minhook       # for GameInjector (enumerate modules, etc.)
)

# Windows system libraries
target_link_libraries(AetherAim PRIVATE
    user32.lib
    gdi32.lib
    winmm.lib
    dwmapi.lib
    shell32.lib
    advapi32.lib
)

if(AETHER_USE_SQLITE)
    target_link_libraries(AetherAim PRIVATE sqlite3)
    target_compile_definitions(AetherAim PRIVATE AETHER_HAS_SQLITE)
endif()

# ============================================================================
# Application icon
# ============================================================================
# If assets/icon.ico exists, embed it as the application icon
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.ico")
    target_sources(AetherAim PRIVATE "${CMAKE_SOURCE_DIR}/assets/app.rc")
    target_compile_definitions(AetherAim PRIVATE HAS_CUSTOM_ICON)
    message(STATUS "Custom icon found: assets/icon.ico")
else()
    message(STATUS "No custom icon (assets/icon.ico not found) — using system default")
    # Still compile the .rc for version info
    target_sources(AetherAim PRIVATE "${CMAKE_SOURCE_DIR}/assets/app.rc")
endif()

# ============================================================================
# Properties (AetherAim.exe)
# ============================================================================
set_target_properties(AetherAim PROPERTIES
    WIN32_EXECUTABLE TRUE
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Copy assets to output directory
add_custom_command(TARGET AetherAim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:AetherAim>/assets
)

# ============================================================================
# AetherAimHook.dll — Injected Raw Input Filter DLL
#
// This DLL is injected into target game processes to intercept
// GetRawInputData / GetRawInputBuffer calls via MinHook.
// It reads filter parameters from shared memory (file mapping).
// ============================================================================
add_library(AetherAimHook SHARED
    hook/AetherAimHook.cpp
)

target_include_directories(AetherAimHook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(AetherAimHook PRIVATE
    aether_core       # OneEuroFilter, EMA, Kalman
    aether_utils      # Logger, MathUtils
    minhook           # API hooking
)

target_link_libraries(AetherAimHook PRIVATE
    user32.lib
)

target_compile_definitions(AetherAimHook PRIVATE
    AETHER_HOOK_DLL
    AETHER_BUILD_HOOK_DLL
)

set_target_properties(AetherAimHook PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    PREFIX ""         # No "lib" prefix
)

# Copy hook DLL next to the exe so GameInjector finds it
add_custom_command(TARGET AetherAimHook POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:AetherAimHook>
        $<TARGET_FILE_DIR:AetherAim>/$<TARGET_FILE_NAME:AetherAimHook>
)
