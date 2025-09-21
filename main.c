#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "tusb.h"

// Zkompilovaný PIO program
#include "i2s_rx.pio.h"

// --- Konfigurace ---
#define SAMPLE_RATE 16000
#define NUM_CHANNELS 6
#define SAMPLE_DEPTH 16 // 16 bitů

// GPIO piny podle zapojení
#define PIN_I2S_SCLK 1
#define PIN_I2S_WS   0
#define PIN_I2S_DATA_1 2 // Kanály 1, 2
#define PIN_I2S_DATA_2 3 // Kanály 3, 4
#define PIN_I2S_DATA_3 4 // Kanály 5, 6

// Velikost bufferu - musí být dostatečně velká
// TinyUSB odesílá 1ms audio paketů najednou
#define SAMPLES_PER_PACKET (SAMPLE_RATE / 1000)
#define CAPTURE_BUFFER_SIZE (SAMPLES_PER_PACKET * 2) // Dvojitý buffer

// Buffery pro zachytávání dat z DMA
// Data z I2S jsou 32-bitová (i když používáme jen 16), proto int32_t
int32_t capture_buf_ch12[CAPTURE_BUFFER_SIZE];
int32_t capture_buf_ch34[CAPTURE_BUFFER_SIZE];
int32_t capture_buf_ch56[CAPTURE_BUFFER_SIZE];

// Buffer pro odeslání přes USB (prokládaná data)
int16_t usb_buf[SAMPLES_PER_PACKET * NUM_CHANNELS];

// DMA kanály
int dma_chan_12, dma_chan_34, dma_chan_56;

// Funkce pro inicializaci jednoho I2S přijímače pomocí PIO
void init_i2s_pio(PIO pio, uint sm, uint pio_program_offset, uint data_pin, uint sclk_pin) {
    i2s_rx_program_init(pio, sm, pio_program_offset, data_pin, sclk_pin);
}

// Funkce pro inicializaci DMA pro jeden PIO přijímač
void init_dma_channel(int* dma_channel, PIO pio, uint sm, int32_t* buffer, size_t buffer_size) {
    *dma_channel = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(*dma_channel);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(
        *dma_channel,
        &c,
        buffer,                 // Cílová adresa (náš buffer)
        &pio->rxf[sm],          // Zdrojová adresa (PIO RX FIFO)
        buffer_size,
        true                    // Spustit okamžitě
    );
}

// Callback volaný, když TinyUSB potřebuje další audio data
void tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)itf;
    (void)ep_in;
    (void)cur_alt_setting;

    // Prokládání dat z jednotlivých bufferů do jednoho USB bufferu
    for (int i = 0; i < SAMPLES_PER_PACKET; i++) {
        // I2S data jsou MSB zarovnaná, takže pro 16-bit vzorek stačí posunout
        // a vzít horních 16 bitů z 32-bitového PIO slova.
        int16_t sample1 = (int16_t)(capture_buf_ch12[i] >> (32 - SAMPLE_DEPTH));
        int16_t sample2 = (int16_t)(capture_buf_ch12[i+SAMPLES_PER_PACKET] >> (32 - SAMPLE_DEPTH)); // Data z druhého kanálu
        
        int16_t sample3 = (int16_t)(capture_buf_ch34[i] >> (32 - SAMPLE_DEPTH));
        int16_t sample4 = (int16_t)(capture_buf_ch34[i+SAMPLES_PER_PACKET] >> (32 - SAMPLE_DEPTH));

        int16_t sample5 = (int16_t)(capture_buf_ch56[i] >> (32 - SAMPLE_DEPTH));
        int16_t sample6 = (int16_t)(capture_buf_ch56[i+SAMPLES_PER_PACKET] >> (32 - SAMPLE_DEPTH));

        int base_idx = i * NUM_CHANNELS;
        usb_buf[base_idx + 0] = sample1;
        usb_buf[base_idx + 1] = sample2;
        usb_buf[base_idx + 2] = sample3;
        usb_buf[base_idx + 3] = sample4;
        usb_buf[base_idx + 4] = sample5;
        usb_buf[base_idx + 5] = sample6;
    }

    tud_audio_write(usb_buf, sizeof(usb_buf));
}


int main() {
    stdio_init_all();

    // Nastavení systémového taktu pro přesné audio frekvence
    // 16000 Hz * 2 (stereo) * 32 bitů * 2 (PIO instrukce na bit) = 2.048 MHz
    // Použijeme systémový PLL
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t pio_clk_div = sys_clk * 2 / (SAMPLE_RATE * 32 * 2); 
    // Poznámka: Tato kalkulace je zjednodušená. Pro production-grade
    // audio je potřeba přesnější nastavení PLL.

    // Inicializace TinyUSB
    tusb_init();

    // --- Konfigurace PIO ---
    PIO pio = pio0;
    uint pio_program_offset = pio_add_program(pio, &i2s_rx_program);
    
    // Inicializace tří PIO State Machines pro tři datové linky
    // Sdílejí SCLK a WS, ale každá má svůj DATA pin
    init_i2s_pio(pio, 0, pio_program_offset, PIN_I2S_DATA_1, PIN_I2S_SCLK);
    init_i2s_pio(pio, 1, pio_program_offset, PIN_I2S_DATA_2, PIN_I2S_SCLK);
    init_i2s_pio(pio, 2, pio_program_offset, PIN_I2S_DATA_3, PIN_I2S_SCLK);

    // --- Konfigurace DMA ---
    init_dma_channel(&dma_chan_12, pio, 0, capture_buf_ch12, CAPTURE_BUFFER_SIZE);
    init_dma_channel(&dma_chan_34, pio, 1, capture_buf_ch34, CAPTURE_BUFFER_SIZE);
    init_dma_channel(&dma_chan_56, pio, 2, capture_buf_ch56, CAPTURE_BUFFER_SIZE);
    
    // Spuštění všech PIO State Machines současně
    pio_sm_set_enabled(pio, 0, true);
    pio_sm_set_enabled(pio, 1, true);
    pio_sm_set_enabled(pio, 2, true);

    printf("6-kanálová zvuková karta spuštěna.\n");

    while (1) {
        tud_task(); // TinyUSB task
    }

    return 0;
}

