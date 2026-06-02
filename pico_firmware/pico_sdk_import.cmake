# This file should be replaced by the one from pico-sdk:
#   cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .
#
# It is included automatically if found.
# If missing, CMakeLists.txt will try $PICO_SDK_PATH/external/pico_sdk_import.cmake
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    endif()
endif()

if(PICO_SDK_PATH)
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
else()
    message(FATAL_ERROR
        "Pico SDK not found.\n"
        "Set PICO_SDK_PATH or download pico-sdk and copy pico_sdk_import.cmake here.\n"
        "  git clone https://github.com/raspberrypi/pico-sdk.git\n"
        "  export PICO_SDK_PATH=$(pwd)/pico-sdk\n"
        "  cp $PICO_SDK_PATH/external/pico_sdk_import.cmake pico_firmware/")
endif()
