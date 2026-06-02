# Config and profile management library
add_library(aether_config INTERFACE)
target_include_directories(aether_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aether_config INTERFACE nlohmann_json::nlohmann_json)
