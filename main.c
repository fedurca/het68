#include "i2s_rx.pio.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "tusb.h"

// --- Audio parameters derived from tusb_config.h ---
#define AUDIO_SAMPLE_RATE   CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
#define AUDIO_N_CHANNELS    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
#define AUDIO_SAMPLE_BYTES  CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX
#define AUDIO_PACKET_SIZE   (AUDIO_SAMPLE_RATE / 1000 * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES)

// --- GPIO DEFINES ---
#define PIN_I2S_DATA 2
#define PIN_I2S_SCK  3
#define PIN_I2S_WS   4

// --- DMA DEFINES ---
#define DMA_ADC_CHANNEL 0
#define DMA_BUF_SIZE_IN_BYTES (AUDIO_PACKET_SIZE * 2) // Double-buffer

// Buffers
static uint8_t dma_buf[DMA_BUF_SIZE_IN_BYTES];
static uint8_t usb_frame_buf[AUDIO_PACKET_SIZE];
volatile int dma_buf_select = 0;

void dma_handler() {
    dma_irqn_acknowledge_channel(DMA_IRQ_0, DMA_ADC_CHANNEL);
    dma_buf_select = 1 - dma_buf_select;
    uint32_t next_dma_addr = (uint32_t)dma_buf + (dma_buf_select * AUDIO_PACKET_SIZE);
    dma_channel_set_write_addr(DMA_ADC_CHANNEL, (void*)next_dma_addr, true);
}

int main(void) {
    stdio_init_all();
    tusb_init();

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);
    uint32_t heartbeat_interval_ms = 200;
    absolute_time_t next_heartbeat = make_timeout_time_ms(heartbeat_interval_ms);
    bool led_state = false;

    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_rx_program);
    i2s_rx_program_init(pio, sm, offset, PIN_I2S_DATA, PIN_I2S_SCK, PIN_I2S_WS, AUDIO_SAMPLE_RATE);

    dma_channel_config c = dma_channel_get_default_config(DMA_ADC_CHANNEL);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(DMA_ADC_CHANNEL, &c, dma_buf, &pio->rxf[sm], DMA_BUF_SIZE_IN_BYTES / 4, false);

    dma_channel_set_irq0_enabled(DMA_ADC_CHANNEL, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    pio_sm_set_enabled(pio, sm, true);
    dma_channel_start(DMA_ADC_CHANNEL);

    while (1) {
        tud_task();
        if (absolute_time_min(get_absolute_time(), next_heartbeat) == next_heartbeat) {
            led_state = !led_state;
            gpio_put(led_pin, led_state);
            next_heartbeat = make_timeout_time_ms(heartbeat_interval_ms);
        }
    }
    return 0;
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
    if (!tud_audio_n_mounted(func_id)) return false;

    uint32_t filled_dma_buf_addr = (uint32_t)dma_buf + ((1 - dma_buf_select) * AUDIO_PACKET_SIZE);
    memcpy(usb_frame_buf, (void*)filled_dma_buf_addr, sizeof(usb_frame_buf));
    (void)tud_audio_write(usb_frame_buf, sizeof(usb_frame_buf));
    
    return true;
}
