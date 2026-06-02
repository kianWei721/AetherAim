cmake_minimum_required(VERSION 3.13)

# ============================================================================
# AetherAim Pico HID Proxy — Build Configuration
#
# Prerequisites:
#   1. Raspberry Pi Pico SDK
#      git clone https://github.com/raspberrypi/pico-sdk.git
#      Set PICO_SDK_PATH environment variable or pass via -D
#
#   2. Pico-PIO-USB library (USB Host via PIO)
#      git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git
#      Place in lib/pico-pio-usb or set PICO_PIO_USB_PATH
#
# Build:
#   mkdir build && cd build
#   cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
#   make -j4
#
# Flash:
#   Hold BOOTSEL button on Pico while plugging in USB
#   Copy aether_pico.uf2 to the RPI-RP2 drive
# ============================================================================

# ── Pico SDK setup ─────────────────────────────────────────────────────
# Include the SDK import script (provided by pico-sdk)
include(${CMAKE_SOURCE_DIR}/pico_sdk_import.cmake OPTIONAL
    RESULT_VARIABLE PICO_SDK_IMPORTED)
if(NOT PICO_SDK_IMPORTED)
    # Try to find pico-sdk via PICO_SDK_PATH
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    endif()
    if(PICO_SDK_PATH)
        include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    else()
        message(FATAL_ERROR
            "Pico SDK not found.\n"
            "Set PICO_SDK_PATH environment variable or pass -DPICO_SDK_PATH=/path/to/pico-sdk\n"
            "Download: git clone https://github.com/raspberrypi/pico-sdk.git")
    endif()
endif()

project(aether_pico C CXX)

# Initialize SDK (must come before add_executable)
pico_sdk_init()

# ============================================================================
# Pico-PIO-USB library (USB Host via PIO)
# ============================================================================
# Pico-PIO-USB provides USB Host capability via PIO state machines.
# It bit-bangs USB full-speed (12Mbps) using the RP2040's programmable I/O.
#
# This is the critical piece that allows the Pico to act as a USB Host
# and receive raw mouse HID reports from the physical mouse.
#
# Download: git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git lib/pico-pio-usb
# ============================================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/lib/pico-pio-usb")
    add_subdirectory(lib/pico-pio-usb)
else()
    message(FATAL_ERROR
        "Pico-PIO-USB library not found at lib/pico-pio-usb\n\n"
        "Download it:\n"
        "  cd pico_firmware\n"
        "  mkdir -p lib\n"
        "  git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB.git lib/pico-pio-usb\n\n"
        "Or set PICO_PIO_USB_PATH to the library location.")
endif()

# ============================================================================
# AetherAim Pico Firmware
# ============================================================================
add_executable(aether_pico
    src/main.cpp
)

target_include_directories(aether_pico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(aether_pico PRIVATE
    # Pico SDK
    pico_stdlib
    pico_multicore

    # USB Device (Pico → PC, TinyUSB built into SDK)
    tinyusb_device

    # USB Host (Mouse → Pico, Pico-PIO-USB)
    pico_pio_usb
)

# Enable C++ exceptions (needed for std::clamp, std::abs)
target_compile_options(aether_pico PRIVATE
    -Wall -Wextra
    -fno-rtti           # No RTTI (saves flash)
    -fno-exceptions     # No exceptions (embedded best practice)
)

# ── USB stack configuration ───────────────────────────────────────────
target_compile_definitions(aether_pico PRIVATE
    # TinyUSB: enable HID device class
    CFG_TUD_ENABLED=1
    CFG_TUD_HID=1
)

# ── Output formats ────────────────────────────────────────────────────
pico_add_extra_outputs(aether_pico)

# ── Post-build: print flash instructions ──────────────────────────────
add_custom_command(TARGET aether_pico POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "============================================"
    COMMAND ${CMAKE_COMMAND} -E echo "  Build complete!"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "  To flash:"
    COMMAND ${CMAKE_COMMAND} -E echo "  1. Hold BOOTSEL button on Pico"
    COMMAND ${CMAKE_COMMAND} -E echo "  2. Plug Pico into PC via USB"
    COMMAND ${CMAKE_COMMAND} -E echo "  3. Copy aether_pico.uf2 to RPI-RP2 drive"
    COMMAND ${CMAKE_COMMAND} -E echo "============================================"
)
