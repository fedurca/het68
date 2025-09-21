add_library(pio_i2s_rx INTERFACE)

# Generování C hlavičkového souboru z .pio souboru
pico_generate_pio_header(pio_i2s_rx ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

target_link_libraries(pio_i2s_rx INTERFACE pico_stdlib)

