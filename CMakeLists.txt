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
    buzzer.c
    core1_launch.c
    doa.c
    entity_store.c
    detection_log.c
    het68_time.c
    cli.c
    remote_id.c
)

add_executable(pico_6mic_soundcard ${SRCS})

# Version stamp (UART help / docs). Semver core for releases.
add_compile_definitions(HET68_VERSION_STR=\"1.1.0\")

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

# UART debug on Grove Shield UART0: GP0 TX, GP1 RX (header pins 1/2; GND pin 3).
# I2S clocks are on GP8/GP9 (Grove UART1) — no conflict.
add_compile_definitions(
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)
if(HET68_USB_DIAG)
    add_compile_definitions(HET68_USB_DIAG=1)
    message(STATUS "het68: USB DIAG mode (simulated tone) — UART debug on GP0/GP1 (Grove UART0)")
else()
    message(STATUS "het68: I2S mic mode — UART debug on GP0/GP1 (Grove UART0)")
endif()

# Diagnostic: mirror debug output to the Pico's own USB CDC (ttyACM1) in
# addition to the probe UART. Default OFF — the probe UART is the freeze-proof
# primary channel. Enable to isolate firmware vs. UART wiring:
#   HET68_DEBUG_CDC=1 ./build.sh
option(HET68_DEBUG_CDC "Mirror debug to Pico USB CDC (ttyACM1)" OFF)
if(HET68_DEBUG_CDC)
    add_compile_definitions(HET68_DEBUG_CDC=1)
endif()

# One-shot lab sweep for minimum core1 settle delay after OpenOCD flash:
#   HET68_CORE1_SETTLE_SCAN=ON ./build.sh && ./upload.sh
# Optional override (ms): HET68_CORE1_SETTLE_MS=0 ./build.sh
option(HET68_CORE1_SETTLE_SCAN "Run core1 settle delay scan at boot" OFF)
if(HET68_CORE1_SETTLE_SCAN)
    add_compile_definitions(HET68_CORE1_SETTLE_SCAN=1)
    message(STATUS "het68: core1 settle scan mode (no audio/DOA)")
endif()
if(DEFINED HET68_CORE1_SETTLE_MS)
    add_compile_definitions(HET68_CORE1_SETTLE_MS=${HET68_CORE1_SETTLE_MS})
    message(STATUS "het68: core1 settle ms override=${HET68_CORE1_SETTLE_MS}")
endif()

# Acoustic array cube edge length in millimetres. Any integer edge is supported
# (e.g. 50/100/128/150/200/256/512); DOA_MAXLAG and the comparison window
# auto-derive from it in doa.c. Default (unset) is 512 mm.
#   HET68_DOA_EDGE_MM=150 ./build.sh
if(DEFINED HET68_DOA_EDGE_MM)
    add_compile_definitions(HET68_DOA_EDGE_MM=${HET68_DOA_EDGE_MM})
    message(STATUS "het68: DOA cube edge=${HET68_DOA_EDGE_MM} mm")
endif()
# Optional array centre height above ground (mm) for human ground (x,y) projection.
# Default in doa.c is edge * √3/2 (vertex-down cube sitting on the ground).
#   HET68_DOA_HEIGHT_MM=1000 ./build.sh
if(DEFINED HET68_DOA_HEIGHT_MM)
    add_compile_definitions(HET68_DOA_HEIGHT_MM=${HET68_DOA_HEIGHT_MM})
    message(STATUS "het68: DOA array height=${HET68_DOA_HEIGHT_MM} mm")
endif()

# Wrap panic at link time as belt-and-suspenders: some pico-sdk libraries may
# have been pre-built without PICO_PANIC_FUNCTION, so --wrap catches those too.
target_link_options(pico_6mic_soundcard PRIVATE -Wl,--wrap=panic)

target_link_libraries(pico_6mic_soundcard
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_flash
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_pwm
    tinyusb_device
    tinyusb_board
)

# OpenDroneID BLE scanner on CYW43 boards (Pico W / Pico Plus 2 W).
# Relocate BTstack TLV flash bank so it does not collide with DET/ENT ACID slots
# (last 4 sectors): BT uses sectors [-6,-5], DET [-4,-3], ENT [-2,-1].
if (PICO_CYW43_SUPPORTED)
    message(STATUS "het68: CYW43 board — enabling BLE OpenDroneID (HET68_BT_RID)")
    target_compile_definitions(pico_6mic_soundcard PRIVATE
        HET68_BT_RID=1
        # 6 * 4096 — keep expression numeric; FLASH_SECTOR_SIZE is 4096 on RP2
        PICO_FLASH_BANK_TOTAL_SIZE=8192
        PICO_FLASH_BANK_STORAGE_OFFSET=\(PICO_FLASH_SIZE_BYTES-24576\)
    )
    target_link_libraries(pico_6mic_soundcard
        pico_cyw43_arch_none
        pico_btstack_ble
        pico_btstack_cyw43
    )
    target_include_directories(pico_6mic_soundcard PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}  # btstack_config.h
    )
else()
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_BT_RID=0)
endif()

# core1 stack lives in SCRATCH_X (4 KiB on RP2350); default 2 KiB is enough because
# DOA keeps large working buffers static (see s_corr in doa.c).

# vyber si jednu z následujících dvou řádek:
pico_enable_stdio_usb(pico_6mic_soundcard 0)
pico_enable_stdio_uart(pico_6mic_soundcard 0)
pico_add_extra_outputs(pico_6mic_soundcard)