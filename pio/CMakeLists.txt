cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(het68 C CXX ASM)
pico_sdk_init()

add_executable(het68 main.c)  # + tvoje zdroje
target_link_libraries(het68 pico_stdlib hardware_pio hardware_dma hardware_clocks tinyusb_device tinyusb_board)
pico_enable_stdio_usb(het68 1)
pico_enable_stdio_uart(het68 0)
pico_add_extra_outputs(het68)
