cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Pico SDK board" FORCE)
endif()

if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Embedded firmware is performance-sensitive (interpreter dispatch, software
# double arithmetic, LCD transfers). Keep a normal Release build by default;
# developers can still override CMAKE_BUILD_TYPE explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(retrominibasic_picocalc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(RMB_VERSION "0.85" CACHE STRING "Cala's Pokecom BASIC version")
set(RMB_BUILD_NUMBER "local" CACHE STRING "Cala's Pokecom BASIC build number")
option(RMB_ENABLE_LTO "Enable link-time optimization for release benchmarking" OFF)

pico_sdk_init()

# PicoCalc's own SD boot code uses pico-vfs. Pin the same known-good revision
# instead of following the dependency's moving main branch.
include(FetchContent)
FetchContent_Declare(
    pico_vfs
    GIT_REPOSITORY https://github.com/oyama/pico-vfs.git
    GIT_TAG 4b71f274acae7de9a3696d3345992294fa9e034e
    GIT_SHALLOW FALSE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_GetProperties(pico_vfs)
if(NOT pico_vfs_POPULATED)
    FetchContent_Populate(pico_vfs)
    add_subdirectory(${pico_vfs_SOURCE_DIR} ${pico_vfs_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

add_executable(retrominibasic_picocalc
    src/main.cpp
    src/core/line_editor.cpp
    src/core/program_store.cpp
    src/core/il.cpp
    src/core/audio_engine.cpp
    src/core/bluetooth_serial_core.cpp
    src/core/basic_compiler.cpp
    src/core/vm.cpp
    src/core/graphics_text.cpp
    src/core/repl.cpp
    src/core/xmodem.cpp
    src/core/ymodem.cpp
    src/core/safe_file.cpp
    src/core/http_file_server_core.cpp
    src/core/transfer_file.cpp
    src/platform/picocalc/platform.cpp
    src/platform/picocalc/audio.cpp
    src/platform/picocalc/external_i2c.cpp
    src/platform/picocalc/serial_transfer.cpp
    src/platform/picocalc/usb_device.cpp
    src/platform/picocalc/usb_descriptors.cpp
    src/platform/picocalc/usb_msc.cpp
    src/platform/picocalc/display.cpp
    src/platform/picocalc/keyboard.cpp
    src/platform/picocalc/network.cpp
    src/platform/picocalc/wireless.cpp
    src/platform/picocalc/bluetooth_serial.cpp
    src/platform/picocalc/file_server.cpp
    src/platform/picocalc/storage.cpp
)

target_include_directories(retrominibasic_picocalc PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Keep reproducible SRAM and static stack evidence in CI artifacts.
target_compile_options(retrominibasic_picocalc PRIVATE -fstack-usage)
target_link_options(retrominibasic_picocalc PRIVATE
    "-Wl,-Map=${CMAKE_BINARY_DIR}/retrominibasic_picocalc.map")

if(RMB_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT RMB_IPO_SUPPORTED OUTPUT RMB_IPO_ERROR)
    if(RMB_IPO_SUPPORTED)
        set_property(
            TARGET retrominibasic_picocalc
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
        )
    else()
        message(WARNING "LTO/IPO unavailable: ${RMB_IPO_ERROR}")
    endif()
endif()

target_compile_definitions(retrominibasic_picocalc PRIVATE
    PICO_VFS_MAX_FAT_VOLUME=1
    PICO_VFS_MAX_MOUNTPOINT=2
    RMB_VERSION="${RMB_VERSION}"
    RMB_BUILD_NUMBER="${RMB_BUILD_NUMBER}"
    # Keep the SDK's CDC driver, init, mutex and IRQ task. Only replace its
    # descriptors; tinyusb_device_unmarked remains owned by pico_stdio_usb.
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=1
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    # Expose the SDK reset interface so picotool -f can move a running CPB
    # directly into BOOTSEL and return it to the application after flashing.
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB=1
    PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=1
    PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF=3
    CFG_TUD_MSC=1
    CFG_TUD_MSC_EP_BUFSIZE=512
)


target_link_libraries(retrominibasic_picocalc
    pico_stdlib
    pico_usb_reset
    pico_unique_id
    pico_low_power
    hardware_clocks
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_dma
    pico_cyw43_arch_lwip_threadsafe_background
    pico_btstack_cyw43
    pico_btstack_classic
    pico_rand
    blockdevice_sd
    filesystem_fat
    filesystem_vfs
)

# Keep USB/UART stdio enabled as a debug fallback during bring-up.
pico_enable_stdio_usb(retrominibasic_picocalc 1)
pico_enable_stdio_uart(retrominibasic_picocalc 1)

pico_add_extra_outputs(retrominibasic_picocalc)

# Keep the internal target stable for tooling while publishing product-facing
# filenames for users and the GitHub Actions artifact.
add_custom_command(TARGET retrominibasic_picocalc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/retrominibasic_picocalc.uf2"
        "${CMAKE_CURRENT_BINARY_DIR}/CPokecombasic.uf2"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:retrominibasic_picocalc>"
        "${CMAKE_CURRENT_BINARY_DIR}/CPokecombasic.elf"
)
