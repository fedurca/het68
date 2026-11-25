cmake_minimum_required(VERSION 3.13)

# vendored SDK
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
include(${CMAKE_CURRENT_LIST_DIR}/pico-sdk/external/pico_sdk_import.cmake)

project(pico_6mic_soundcard C CXX ASM)
pico_sdk_init()

# The wideband detector's static tables do not fit in RP2040 SRAM (264 KB).
# The 6 MiB DOA ring lives in the 8 MB PSRAM on Pico Plus 2 W only.
if(PICO_PLATFORM STREQUAL "rp2040")
    message(FATAL_ERROR
        "het68 requires the Pimoroni Pico Plus 2 W (RP2350B + 8 MB PSRAM). "
        "RP2040 (Pico / Pico W) is not supported.")
endif()
if(NOT PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
    message(FATAL_ERROR
        "het68 supports only PICO_BOARD=pimoroni_pico_plus2_w_rp2350 "
        "(Pimoroni Pico Plus 2 W, 8 MB PSRAM).")
endif()

# TrustZone / H68P options (see docs/secure-boot-lab.md).
option(HET68_TRUSTZONE "Configure SAU region plan at boot" ON)
option(HET68_PLUGIN_EXTERNAL "Load detection from signed H68P into Secure SRAM" OFF)
option(HET68_PLUGIN_REQUIRED "Refuse builtin detection without a valid H68P" OFF)

set(SRCS
    main.c
    usb_descriptors.c
    debug_io.c
    buzzer.c
    core1_launch.c
    entity_store.c
    detection_log.c
    het68_time.c
    cli.c
    remote_id.c
    drone_store.c
    dps310.c
    acoustic_link.c
    node_store.c
    psram.c
    h68p.c
    h68p_pubkey.c
    secure/het68_sau.c
    secure/het68_secure_start.c
    secure/het68_nsc.c
    secure/het68_ns_transition.c
    secure/plugin_flash.c
    secure/mailbox.c
)

# Isolated secp256k1 micro-ecc (renamed symbols) — private include path so
# BTstack's older micro-ecc headers/asm are not picked up.
add_library(h68p_uecc STATIC h68p_uecc_wrap.c)
target_include_directories(h68p_uecc PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/third_party/micro_ecc
)
target_compile_options(h68p_uecc PRIVATE -ffunction-sections -fdata-sections)

if(NOT HET68_PLUGIN_EXTERNAL)
    list(APPEND SRCS doa.c neo_srp.c longwin.c)
endif()

add_executable(pico_6mic_soundcard ${SRCS})

# Version stamp (UART help / docs). Semver core for releases.
string(TIMESTAMP HET68_BUILD_STAMP "%Y-%m-%d %H:%M:%S")
add_compile_definitions(
    HET68_VERSION_STR=\"1.26.1\"
    HET68_BUILD_STAMP=\"${HET68_BUILD_STAMP}\"
)
message(STATUS "het68: version 1.26.0  build ${HET68_BUILD_STAMP}")

# Optional fixed acoustic-link node id (0..7). Default 0; also settable at
# runtime via the LINK ID CLI command.
#   HET68_NODE_ID=3 ./build.sh
if(DEFINED HET68_NODE_ID)
    add_compile_definitions(HET68_NODE_ID=${HET68_NODE_ID})
    message(STATUS "het68: acoustic node id=${HET68_NODE_ID}")
endif()
message(STATUS "het68: PICO_BOARD=${PICO_BOARD} platform=${PICO_PLATFORM}")

# PIO header po add_executable
pico_generate_pio_header(pico_6mic_soundcard ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_rx.pio)
pico_generate_pio_header(pico_6mic_soundcard ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_clk.pio)

target_include_directories(pico_6mic_soundcard PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/secure
    ${CMAKE_CURRENT_BINARY_DIR}
)

# H68P secp256k1 verify via h68p_uecc (renamed micro-ecc) — avoids colliding
# with BTstack's older secp256r1 micro-ecc copy.

if(HET68_TRUSTZONE)
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_TRUSTZONE=1)
else()
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_TRUSTZONE=0)
endif()
# Dual-ELF SAU programming is gated off until NSC veneers exist.
target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_SAU_PROGRAM=0)
if(HET68_PLUGIN_EXTERNAL)
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_PLUGIN_EXTERNAL=1)
    message(STATUS "het68: H68P external detection (no builtin doa link)")
else()
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_PLUGIN_EXTERNAL=0)
endif()
if(HET68_PLUGIN_REQUIRED)
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_PLUGIN_REQUIRED=1)
else()
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_PLUGIN_REQUIRED=0)
endif()

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

# Mirror debug to the Pico's own USB CDC as well as the probe UART, so a
# terminal on either ACM port sees the log. Writes never block: if the CDC
# buffer is full the byte stays on the probe UART only.
#   HET68_DEBUG_CDC=0 ./build.sh
option(HET68_DEBUG_CDC "Mirror debug to the Pico USB CDC serial port" ON)
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
# (e.g. 50/100/128/150/200/256/384/512); DOA_MAXLAG and the comparison window
# auto-derive from it in doa.c. Default (unset) is 128 mm.
#   HET68_DOA_EDGE_MM=512 ./build.sh
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
    h68p_uecc
    pico_stdlib
    pico_unique_id
    pico_multicore
    pico_flash
    hardware_flash
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_pwm
    hardware_i2c
    tinyusb_device
    tinyusb_board
)

# OpenDroneID BLE scanner on the Plus 2 W CYW43 radio.
# Flash end layout (4 KiB sectors): DRONE[-8,-7], BT[-6,-5], DET[-4,-3], ENT[-2,-1].
# Relocate BTstack TLV so it does not collide with DRONE/DET/ENT ACID slots.
if (PICO_CYW43_SUPPORTED)
    message(STATUS "het68: CYW43 board — enabling BLE OpenDroneID (HET68_BT_RID)")
    target_compile_definitions(pico_6mic_soundcard PRIVATE
        HET68_BT_RID=1
        # Acoustic-link Wi-Fi wake can bring up CYW43 STA on these boards.
        HET68_WIFI_WAKE=1
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
    target_compile_definitions(pico_6mic_soundcard PRIVATE HET68_BT_RID=0 HET68_WIFI_WAKE=0)
endif()

# core1 stack lives in SCRATCH_X (4 KiB on RP2350); default 2 KiB is enough because
# DOA keeps large working buffers static (see s_corr in doa.c).

# vyber si jednu z následujících dvou řádek:
pico_enable_stdio_usb(pico_6mic_soundcard 0)
pico_enable_stdio_uart(pico_6mic_soundcard 0)
pico_add_extra_outputs(pico_6mic_soundcard)

# --- H68P detection stub payload (per-device signing input) -------------------
find_program(HET68_ARM_GCC arm-none-eabi-gcc)
find_program(HET68_ARM_OBJCOPY arm-none-eabi-objcopy)
if(HET68_ARM_GCC AND HET68_ARM_OBJCOPY)
    set(HET68_DETECT_ELF ${CMAKE_BINARY_DIR}/het68_detect.elf)
    set(HET68_DETECT_BIN ${CMAKE_BINARY_DIR}/het68_detect.bin)
    add_custom_command(
        OUTPUT ${HET68_DETECT_ELF} ${HET68_DETECT_BIN}
        COMMAND ${HET68_ARM_GCC}
            -mcpu=cortex-m33 -mthumb -ffreestanding -nostdlib -Os
            -Wl,-T${CMAKE_CURRENT_LIST_DIR}/plugin/plugin.ld
            -Wl,--nmagic
            -Wl,-e,plugin_entry
            -o ${HET68_DETECT_ELF}
            ${CMAKE_CURRENT_LIST_DIR}/plugin/plugin_stub.c
        COMMAND ${HET68_ARM_OBJCOPY} -O binary ${HET68_DETECT_ELF} ${HET68_DETECT_BIN}
        DEPENDS
            ${CMAKE_CURRENT_LIST_DIR}/plugin/plugin_stub.c
            ${CMAKE_CURRENT_LIST_DIR}/plugin/plugin.ld
        COMMENT "Building H68P stub payload het68_detect.bin"
        VERBATIM
    )
    add_custom_target(het68_detect ALL DEPENDS ${HET68_DETECT_BIN})
    message(STATUS "het68: het68_detect stub payload enabled")
else()
    message(WARNING "het68: arm-none-eabi toolchain not found — skipping het68_detect")
endif()