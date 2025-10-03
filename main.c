// main.c — RP2040 + PIO I2S RX -> TinyUSB audio (TX to host)

#include "i2s_rx.pio.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "tusb.h"

// ---------- Audio parameters (taken from tusb_config.h) ----------
#define AUDIO_SAMPLE_RATE   CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
#define AUDIO_N_CHANNELS    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
#define AUDIO_SAMPLE_BYTES  CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX
#define AUDIO_PACKET_SIZE   (AUDIO_SAMPLE_RATE / 1000 * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES)
// One USB frame (1 ms) worth of audio bytes per packet.

// ---------- Pins ----------
#define PIN_I2S_DATA  2
#define PIN_I2S_SCK   3
#define PIN_I2S_WS    4

// ---------- DMA setup ----------
#define DMA_ADC_CHANNEL  0
// Double buffer: 2 * one-USB-packet so DMA can fill one while USB sends the other
#define DMA_BUF_SIZE_IN_BYTES   (2 * AUDIO_PACKET_SIZE)

// ---------- Globals ----------
static volatile uint32_t dma_buf_select = 0;               // 0 or 1
static uint8_t  usb_frame_buf[AUDIO_PACKET_SIZE];          // staging for tud_audio_write
static uint8_t  dma_buf[DMA_BUF_SIZE_IN_BYTES];            // DMA target (2 halves)

// ---------- Minimal PIO init helper for i2s_rx ----------
static void i2s_rx_program_init(PIO pio, uint sm, uint offset,
                                uint pin_data, uint pin_sck, uint pin_ws,
                                uint32_t sample_rate)
{
    // Configure pins
    pio_gpio_init(pio, pin_sck);
    pio_gpio_init(pio, pin_ws);
    pio_gpio_init(pio, pin_data);

    // SCK (sideset) is output; WS and DATA are inputs
    pio_sm_set_consecutive_pindirs(pio, sm, pin_sck, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_ws,  1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_data,1, false);

    // Build config from program
    pio_sm_config c = i2s_rx_program_get_default_config(offset);

    // Map pins to functions
    sm_config_set_sideset_pins(&c, pin_sck);   // sideset drives SCK
    sm_config_set_in_pins(&c, pin_data);       // IN reads from DATA
    sm_config_set_jmp_pin(&c, pin_ws);         // WAIT/JMP uses WS

    // Shift config: explicit PUSH in PIO program -> no autopush
    sm_config_set_in_shift(&c, true /*right*/, false /*autopush*/, 32);

    // Clocking:
    // PIO toggles SCK every instruction; one data bit uses 2 instr (in+nop)
    // => f_sm = 2 * BCLK. For 16-bit stereo: BCLK = 32 * fs. => f_sm = 64 * fs.
    float div = (float)clock_get_hz(clk_sys) / (64.0f * (float)sample_rate);
    sm_config_set_clkdiv(&c, div);

    // Init SM
    pio_sm_init(pio, sm, offset, &c);
}

// ---------- DMA IRQ handler ----------
static void __isr dma_handler(void)
{
    // Ack IRQ for our channel
    dma_hw->ints0 = 1u << DMA_ADC_CHANNEL;

    // Flip buffer half
    dma_buf_select ^= 1;

    // Point DMA to the other half of our buffer and re-trigger
    uint32_t next_addr = (uint32_t)dma_buf + (dma_buf_select * AUDIO_PACKET_SIZE);
    dma_channel_set_write_addr(DMA_ADC_CHANNEL, (void *)next_addr, true);
}

// ---------- TinyUSB audio callback ----------
// Called before TinyUSB loads the IN endpoint; we copy the just-filled half.
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id,
                                   uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
    if (!tud_audio_n_mounted(func_id)) return false;

    uint32_t filled_addr = (uint32_t)dma_buf + ((1u - dma_buf_select) * AUDIO_PACKET_SIZE);
    memcpy(usb_frame_buf, (void *)filled_addr, sizeof(usb_frame_buf));
    (void)tud_audio_write(usb_frame_buf, sizeof(usb_frame_buf));
    return true;
}

int main(void)
{
    stdio_init_all();
    tusb_init();

    // Heartbeat LED
    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);
    bool led_state = false;
    uint32_t heartbeat_ms = 200;
    absolute_time_t next_heartbeat = make_timeout_time_ms(heartbeat_ms);

    // -------- PIO program load & init --------
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_rx_program);
    i2s_rx_program_init(pio, sm, offset, PIN_I2S_DATA, PIN_I2S_SCK, PIN_I2S_WS, AUDIO_SAMPLE_RATE);

    // Enable SM
    pio_sm_set_enabled(pio, sm, true);

    // -------- DMA config --------
    dma_channel_config c = dma_channel_get_default_config(DMA_ADC_CHANNEL);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);       // PIO RX FIFO is 32-bit
    channel_config_set_read_increment(&c, false);                  // read from fixed FIFO address
    channel_config_set_write_increment(&c, true);                  // write linear buffer
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));     // PIO RX DREQ

    // Start writing into the first half
    dma_buf_select = 0;
    dma_channel_configure(
        DMA_ADC_CHANNEL, &c,
        dma_buf,               // dst
        &pio->rxf[sm],         // src
        AUDIO_PACKET_SIZE / 4, // 32-bit words per half
        false                  // don't start yet
    );

    // IRQ for half-complete
    dma_channel_set_irq0_enabled(DMA_ADC_CHANNEL, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Kick off first transfer
    dma_channel_start(DMA_ADC_CHANNEL);

    // -------- Main loop --------
    for (;;)
    {
        tud_task();

        // Blink
        if (absolute_time_diff_us(get_absolute_time(), next_heartbeat) <= 0)
        {
            led_state = !led_state;
            gpio_put(led_pin, led_state);
            next_heartbeat = make_timeout_time_ms(heartbeat_ms);
        }
    }
}

