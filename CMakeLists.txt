cmake_minimum_required(VERSION 3.13)

# vendored SDK
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
include(${CMAKE_CURRENT_LIST_DIR}/pico-sdk/external/pico_sdk_import.cmake)

project(pico_6mic_soundcard C CXX ASM)
pico_sdk_init()

set(SRCS
    main.c
    usb_descriptors.c
    debug_io.c
)

add_executable(pico_6mic_soundcard ${SRCS})

# PIO header po add_executable
pico_generate_pio_header(pico_6mic_soundcard ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_rx.pio)
pico_generate_pio_header(pico_6mic_soundcard ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_clk.pio)

target_include_directories(pico_6mic_soundcard PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

add_compile_definitions(PICO_PANIC_FUNCTION=het68_panic)

# DIAG mode: bypass I2S and emit a simulated 1 kHz tone (no mics required).
#   HET68_USB_DIAG=ON ./build.sh
option(HET68_USB_DIAG "Bypass I2S, emit simulated tone" OFF)

# UART debug on GP8/GP9 (physical pins 11/12; GND on pin 13). These are UART1
# pins and do not conflict with I2S on GP0–GP4. Same wiring for probe and mics.
add_compile_definitions(
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=8
    PICO_DEFAULT_UART_RX_PIN=9
)
if(HET68_USB_DIAG)
    add_compile_definitions(HET68_USB_DIAG=1)
    message(STATUS "het68: USB DIAG mode (simulated tone) — UART debug on GP8/GP9 (pins 11/12)")
else()
    message(STATUS "het68: I2S mic mode — UART debug on GP8/GP9 (pins 11/12)")
endif()

# Diagnostic: mirror debug output to the Pico's own USB CDC (ttyACM1) in
# addition to the probe UART. Default OFF — the probe UART is the freeze-proof
# primary channel. Enable to isolate firmware vs. UART wiring:
#   HET68_DEBUG_CDC=1 ./build.sh
option(HET68_DEBUG_CDC "Mirror debug to Pico USB CDC (ttyACM1)" OFF)
if(HET68_DEBUG_CDC)
    add_compile_definitions(HET68_DEBUG_CDC=1)
endif()

# Wrap panic at link time as belt-and-suspenders: some pico-sdk libraries may
# have been pre-built without PICO_PANIC_FUNCTION, so --wrap catches those too.
target_link_options(pico_6mic_soundcard PRIVATE -Wl,--wrap=panic)

target_link_libraries(pico_6mic_soundcard
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    tinyusb_device
    tinyusb_board
)

# vyber si jednu z následujících dvou řádek:
pico_enable_stdio_usb(pico_6mic_soundcard 0)
pico_enable_stdio_uart(pico_6mic_soundcard 0)
pico_add_extra_outputs(pico_6mic_soundcard)