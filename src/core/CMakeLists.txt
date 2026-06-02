# Core filter library — STATIC so both AetherAim.exe and AetherAimHook.dll
# can link against the same filter implementation.
add_library(aether_core STATIC
    OneEuroFilter.cpp
)
target_include_directories(aether_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src      # for utils/ includes
)
target_link_libraries(aether_core PUBLIC aether_utils)
target_compile_definitions(aether_core PRIVATE
    $<$<BOOL:${AETHER_BUILD_HOOK_DLL}>:AETHER_HOOK_DLL>
)
