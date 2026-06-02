# Dear ImGui GUI layer
add_library(aether_gui INTERFACE)
target_include_directories(aether_gui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aether_gui INTERFACE imgui implot aether_config aether_core)
