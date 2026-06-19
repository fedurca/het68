cmake_minimum_required(VERSION 3.13)

# vendored SDK
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
include(${CMAKE_CURRENT_LIST_DIR}/pico-sdk/external/pico_sdk_import.cmake)

project(pico_6mic_soundcard C CXX ASM)
pico_sdk_init()

set(SRCS
    main.c
    usb_descriptors.c
)

add_executable(pico_6mic_soundcard ${SRCS})

# PIO header po add_executable
pico_generate_pio_header(pico_6mic_soundcard ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_rx.pio)

target_include_directories(pico_6mic_soundcard PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Redirect panic() to our custom handler so UART drains before CPU halts.
# Without this, __breakpoint() in the stock panic halts the CPU (Debug Probe)
# before the UART FIFO transmits the message.
target_compile_definitions(pico_6mic_soundcard PRIVATE
    PICO_PANIC_FUNCTION=het68_panic
)

target_link_libraries(pico_6mic_soundcard
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    tinyusb_device
    tinyusb_board
)

# vyber si jednu z následujících dvou řádek:
pico_enable_stdio_usb(pico_6mic_soundcard 0)   # žádné stdio přes USB -> bez warningu
# pico_enable_stdio_usb(pico_6mic_soundcard 1) # pokud chceš stdio USB, zapni zároveň CFG_TUD_CDC v tusb_config.h

# UART stdio na GP0(TX)/GP1(RX) @ 115200 Bd — pouze pro ladění, I2S ještě není zapojený
pico_enable_stdio_uart(pico_6mic_soundcard 1)
pico_add_extra_outputs(pico_6mic_soundcard)