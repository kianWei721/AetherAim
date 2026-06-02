# Unit tests (Catch2)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
)

# ============================================================================
# Filter tests (OneEuro, EMA, Kalman, 2D)
# ============================================================================
add_executable(test_filters
    test_filters.cpp
    ${CMAKE_SOURCE_DIR}/src/core/OneEuroFilter.cpp
)
target_include_directories(test_filters PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_filters PRIVATE Catch2::Catch2WithMain)
add_test(NAME FilterTests COMMAND test_filters)

# ============================================================================
# ConfigManager tests
# ============================================================================
add_executable(test_config
    test_config.cpp
    ${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)
target_include_directories(test_config PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_config PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)
target_compile_definitions(test_config PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME ConfigTests COMMAND test_config)

# ============================================================================
# ProfileManager tests
# ============================================================================
add_executable(test_profile
    test_profile.cpp
    ${CMAKE_SOURCE_DIR}/src/config/ProfileManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/OneEuroFilter.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)
target_include_directories(test_profile PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_profile PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)
target_compile_definitions(test_profile PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME ProfileTests COMMAND test_profile)

# ============================================================================
# Ring buffer tests (standalone — only needs the header)
# ============================================================================
add_executable(test_ringbuffer
    test_ringbuffer.cpp
)
target_include_directories(test_ringbuffer PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_ringbuffer PRIVATE Catch2::Catch2WithMain)
target_compile_definitions(test_ringbuffer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME RingBufferTests COMMAND test_ringbuffer)

# ============================================================================
# Performance benchmarks (standalone executable, not run via ctest by default)
# ============================================================================
add_executable(benchmark
    benchmark_main.cpp
    ${CMAKE_SOURCE_DIR}/src/core/OneEuroFilter.cpp
)
target_include_directories(benchmark PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# Run with: build\tests\Release\benchmark.exe
