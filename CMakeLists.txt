cmake_minimum_required(VERSION 3.13)

# Soubor .vscode/settings.json specifikuje CMAKE_TOOLCHAIN_FILE.
# Tento soubor automaticky najde a zahrne Pico SDK.
# Můžeme tedy rovnou definovat náš projekt a poté SDK inicializovat.

project(pico_6mic_soundcard C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Inicializace Pico SDK.
# Tato funkce je zpřístupněna souborem toolchain z SDK.
pico_sdk_init()

# Informace o projektu
message(STATUS "Project pico_6mic_soundcard")
message(STATUS "Build type is ${CMAKE_BUILD_TYPE}")

# Přidání PIO souboru jako zdroje
pico_generate_pio_header(pico_6mic_soundcard ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_rx.pio)

# Definice spustitelného souboru
add_executable(pico_6mic_soundcard
    main.c
)

# Propojení s potřebnými knihovnami z SDK
target_link_libraries(pico_6mic_soundcard PRIVATE
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_irq
    # TinyUSB pro USB audio
    tinyusb_device
    tinyusb_board
)

# Zapnutí USB a výstupu na sériový port (pro ladění)
pico_enable_stdio_usb(pico_6mic_soundcard 1)
pico_enable_stdio_uart(pico_6mic_soundcard 0)

# Vytvoření .uf2 souboru pro nahrání
pico_add_extra_outputs(pico_6mic_soundcard)

message(STATUS "Build configuration complete.")

