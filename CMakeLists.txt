cmake_minimum_required(VERSION 3.13)

# Inicializace Pico SDK
# Cesta k SDK musí být nastavena v proměnné prostředí PICO_SDK_PATH
include(pico_sdk_import.cmake)

project(pico_6mic_soundcard C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Přidání podadresáře s PIO programem
add_subdirectory(pio)

# Definice spustitelného souboru
add_executable(${CMAKE_PROJECT_NAME}
    main.c
    tusb_config.h
)

# Povolení USB a logování přes sériový port (pro ladění)
pico_enable_stdio_usb(${CMAKE_PROJECT_NAME} 1)
pico_enable_stdio_uart(${CMAKE_PROJECT_NAME} 0)

# Propojení s potřebnými knihovnami SDK
target_link_libraries(${CMAKE_PROJECT_NAME}
    pico_stdlib
    hardware_pio
    hardware_dma
    # TinyUSB pro USB audio
    tinyusb_device
    tinyusb_board
)

# Zahrnutí adresáře s PIO programem
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/pio
)

# Vytvoření .uf2 souboru pro nahrání
pico_add_extra_outputs(${CMAKE_PROJECT_NAME})

