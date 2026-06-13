# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(optical_bhang C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# --- optical_bhang_runtime: BIST + detection + waveform capture + WiFi upload ---
add_executable(optical_bhang_runtime
    optical_bhang_runtime.c
    bhang_net.c
    bhang_config.c
    led.c
)
pico_set_program_name(optical_bhang_runtime "optical_bhang_runtime")
pico_set_program_version(optical_bhang_runtime "1.0")
pico_enable_stdio_uart(optical_bhang_runtime 1)
pico_enable_stdio_usb(optical_bhang_runtime 1)
target_link_libraries(optical_bhang_runtime
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_gpio
    hardware_timer
    hardware_uart
    hardware_flash
    pico_flash
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mbedtls
    pico_mbedtls
    pico_lwip_sntp
    m
)
target_include_directories(optical_bhang_runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
# Both cores default to a 2 KB stack; bump to 4 KB (the SCRATCH region maximum)
# for TLS/HTTP call-frame headroom now that networking runs on core1.
#
# PICO_MAX_SHARED_IRQ_HANDLERS is a single pool (total across all IRQs/both
# cores) of shared-IRQ handler chain slots; the SDK default of 4 is exhausted
# once cyw43 (threadsafe_background) claims its slots plus our COMP_OUT raw
# handler, so adding the PUSH_BUTTON raw handler hit hard_assert(free_slot>=0)
# in irq_add_shared_handler -> panic, wedging core0. Bump to 8 for headroom.
target_compile_definitions(optical_bhang_runtime PRIVATE
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000
    PICO_MAX_SHARED_IRQ_HANDLERS=8
)
pico_add_extra_outputs(optical_bhang_runtime)

