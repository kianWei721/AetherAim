# Utility helpers (math, hotkey, logging)
add_library(aether_utils INTERFACE)
target_include_directories(aether_utils INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aether_utils INTERFACE
    user32.lib
    nlohmann_json::nlohmann_json
)
