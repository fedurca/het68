// main.c — RP2350 UAC2 6ch microphone (3× ICS-43434 stereo pairs via PIO I2S RX + DMA)
//
// Pinout (see wiring_and_bom.md):
//   GP0 = WS/LRCLK, GP1 = BCLK/SCK (Pico is I2S master)
//   GP2 = SD pair 1+2, GP3 = SD pair 3+4, GP4 = SD pair 5+6
// UART debug on GP16/GP17 (GP0/GP1 reserved for I2S).

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "tusb.h"
#include "debug_io.h"
#include "i2s_rx.pio.h"

// ---------------------------------------------------------------------------
// Raw UART helpers — safe from fault context (no mutex).
// ---------------------------------------------------------------------------
static void raw_puts(const char *s) {
    while (*s) uart_putc_raw(uart_default, *s++);
}

static void raw_puthex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    raw_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        uart_putc_raw(uart_default, hex[(v >> i) & 0xF]);
}

static void raw_putu32(uint32_t v) {
    if (v == 0) { uart_putc_raw(uart_default, '0'); return; }
    char buf[11];
    int n = 0;
    for (; v; v /= 10) buf[n++] = '0' + (v % 10);
    while (n--) uart_putc_raw(uart_default, buf[n]);
}

static void raw_flush(void) {
    while (!uart_is_writable(uart_default)) { }
    for (volatile int i = 0; i < 12000; i++) { }
}

void __attribute__((noreturn)) het68_panic(const char *fmt, ...) {
    (void)fmt;
    raw_puts("\n!!! PANIC !!! uptime=");
    raw_putu32((uint32_t)(time_us_64() / 1000000));
    raw_puts("s\n");
    raw_flush();
    for (;;) { __asm volatile("nop"); }
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile (
        "tst lr, #4        \n"
        "ite eq            \n"
        "mrseq r0, msp     \n"
        "mrsne r0, psp     \n"
        "b het68_hardfault \n"
        ::: "r0"
    );
}

void __attribute__((noreturn)) het68_hardfault(uint32_t *frame) {
    raw_puts("\n!!! HARDFAULT !!!\nPC=");
    raw_puthex32(frame[6]);
    raw_puts(" LR=");
    raw_puthex32(frame[5]);
    raw_puts("\n");
    raw_flush();
    for (;;) { __asm volatile("nop"); }
}

void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
    (void)fmt;
    raw_puts("\n!!! PANIC(wrap) !!!\n");
    raw_flush();
    for (;;) { __asm volatile("nop"); }
}

// ---------------------------------------------------------------------------
// Audio / USB constants
// ---------------------------------------------------------------------------
#define AUDIO_SAMPLE_RATE   CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
#define AUDIO_N_CHANNELS    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
#define AUDIO_SAMPLE_BYTES  CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX
#define AUDIO_SAMPLES_PER_USB_FRAME  (AUDIO_SAMPLE_RATE / 1000u)
#define AUDIO_PACKET_SIZE   (AUDIO_SAMPLES_PER_USB_FRAME * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES)

#define ID_IT   0x01
#define ID_FU   0x02
#define ID_OT   0x03
#define ID_CLK  0x04

// Set to 1 at compile time to bypass I2S and emit diagnostic tones (USB-only test).
#ifndef HET68_USB_DIAG
#define HET68_USB_DIAG 0
#endif

// ---------------------------------------------------------------------------
// I2S pins — must match wiring_and_bom.md / README_v24.txt
// ---------------------------------------------------------------------------
#define PIN_I2S_WS    0
#define PIN_I2S_SCK   1
#define PIN_I2S_D01   2
#define PIN_I2S_D23   3
#define PIN_I2S_D45   4

#define I2S_NUM_LINES           3u
#define I2S_WORDS_PER_FRAME     (AUDIO_SAMPLES_PER_USB_FRAME * 2u)   // L+R per line
#define I2S_PINGPONG_WORDS      (I2S_WORDS_PER_FRAME * 2u)

static uint8_t usb_frame_buf[AUDIO_PACKET_SIZE] __attribute__((aligned(4)));

static uint32_t current_sample_rate = AUDIO_SAMPLE_RATE;
static uint8_t clock_valid = 1;
static uint8_t master_mute = 0;

// USB debug counters (read in heartbeat; updated from control callbacks only).
static volatile uint32_t dbg_ctrl_clk_valid;
static volatile uint32_t dbg_ctrl_sam_freq;
static volatile uint32_t dbg_ctrl_other;
static volatile uint32_t dbg_set_itf;
static volatile uint32_t dbg_usb_frames;
static volatile uint8_t  dbg_last_alt;

#if HET68_USB_DIAG
static uint32_t diag_frame_counter;
#else
// ---------------------------------------------------------------------------
// I2S capture: 3 PIO SMs + 3 DMA channels, ping-pong per line
// ---------------------------------------------------------------------------
static PIO i2s_pio;
static uint i2s_sm[I2S_NUM_LINES];
static int i2s_dma[I2S_NUM_LINES];
static uint32_t i2s_cap[I2S_NUM_LINES][I2S_PINGPONG_WORDS];
static volatile uint8_t i2s_ready_half;
static volatile bool i2s_frame_ready;
static bool i2s_started;
#endif

#if !HET68_USB_DIAG
static void i2s_sm_init(uint sm, uint offset, uint pin_data, uint32_t sample_rate) {
    pio_gpio_init(i2s_pio, PIN_I2S_SCK);
    pio_gpio_init(i2s_pio, PIN_I2S_WS);
    pio_gpio_init(i2s_pio, pin_data);

    pio_sm_set_consecutive_pindirs(i2s_pio, sm, PIN_I2S_SCK, 1, true);
    pio_sm_set_consecutive_pindirs(i2s_pio, sm, PIN_I2S_WS, 1, false);
    pio_sm_set_consecutive_pindirs(i2s_pio, sm, pin_data, 1, false);

    pio_sm_config c = i2s_rx_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PIN_I2S_SCK);
    sm_config_set_in_pins(&c, pin_data);
    sm_config_set_jmp_pin(&c, PIN_I2S_WS);
    sm_config_set_in_shift(&c, true, false, 32);

    // 16-bit stereo @ fs: BCLK = 32*fs, PIO toggles SCK every instruction → f_sm = 64*fs
    float div = (float)clock_get_hz(clk_sys) / (64.0f * (float)sample_rate);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(i2s_pio, sm, offset, &c);
}

static void i2s_dma_start_half(uint8_t half) {
    uint32_t base_off = (uint32_t)half * I2S_WORDS_PER_FRAME;
    for (uint i = 0; i < I2S_NUM_LINES; i++) {
        dma_channel_set_write_addr(i2s_dma[i], &i2s_cap[i][base_off], false);
        dma_channel_set_trans_count(i2s_dma[i], I2S_WORDS_PER_FRAME, true);
    }
}

static void __isr i2s_dma_irq(void) {
    dma_hw->ints0 = (1u << i2s_dma[0]);
    i2s_ready_half = (uint8_t)(1u - i2s_ready_half);
    i2s_frame_ready = true;
    i2s_dma_start_half(i2s_ready_half);
}

static void i2s_capture_init(uint32_t sample_rate) {
    i2s_pio = pio0;
    uint offset = pio_add_program(i2s_pio, &i2s_rx_program);

    const uint pin_data[I2S_NUM_LINES] = { PIN_I2S_D01, PIN_I2S_D23, PIN_I2S_D45 };

    for (uint i = 0; i < I2S_NUM_LINES; i++) {
        i2s_sm[i] = i;
        i2s_sm_init(i2s_sm[i], offset, pin_data[i], sample_rate);

        i2s_dma[i] = dma_claim_unused_channel(true);
        dma_channel_config c = dma_channel_get_default_config(i2s_dma[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm[i], false));
        dma_channel_configure(
            i2s_dma[i], &c,
            &i2s_cap[i][0],
            &i2s_pio->rxf[i2s_sm[i]],
            I2S_WORDS_PER_FRAME,
            false);
    }

    i2s_ready_half = 0;
    i2s_frame_ready = false;

    dma_channel_set_irq0_enabled(i2s_dma[0], true);
    irq_set_exclusive_handler(DMA_IRQ_0, i2s_dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);

    i2s_dma_start_half(0);

    uint32_t mask = 0;
    for (uint i = 0; i < I2S_NUM_LINES; i++) mask |= (1u << i2s_sm[i]);
    pio_enable_sm_mask_in_sync(i2s_pio, mask);
}

static int16_t i2s_word_to_sample(int32_t raw) {
    // PIO pushes 16-bit MSB-aligned samples in the lower 16 bits of each 32-bit FIFO word.
    return (int16_t)(raw >> 16);
}

static void build_usb_frame_from_i2s(void) {
    uint8_t read_half = (uint8_t)(1u - i2s_ready_half);
    uint32_t base = (uint32_t)read_half * I2S_WORDS_PER_FRAME;

    int16_t *out = (int16_t *)usb_frame_buf;
    for (uint32_t s = 0; s < AUDIO_SAMPLES_PER_USB_FRAME; s++) {
        uint32_t w = base + s * 2u;
        int16_t ch0 = i2s_word_to_sample((int32_t)i2s_cap[0][w]);
        int16_t ch1 = i2s_word_to_sample((int32_t)i2s_cap[0][w + 1u]);
        int16_t ch2 = i2s_word_to_sample((int32_t)i2s_cap[1][w]);
        int16_t ch3 = i2s_word_to_sample((int32_t)i2s_cap[1][w + 1u]);
        int16_t ch4 = i2s_word_to_sample((int32_t)i2s_cap[2][w]);
        int16_t ch5 = i2s_word_to_sample((int32_t)i2s_cap[2][w + 1u]);

        if (master_mute) {
            ch0 = ch1 = ch2 = ch3 = ch4 = ch5 = 0;
        }

        uint32_t o = s * AUDIO_N_CHANNELS;
        out[o + 0] = ch0;
        out[o + 1] = ch1;
        out[o + 2] = ch2;
        out[o + 3] = ch3;
        out[o + 4] = ch4;
        out[o + 5] = ch5;
    }
}
#endif  // !HET68_USB_DIAG

#if HET68_USB_DIAG
static void build_diag_frame(void) {
    int16_t *out = (int16_t *)usb_frame_buf;
    static const uint16_t periods[6] = { 109, 97, 83, 71, 61, 53 };
    static const int16_t amps[6] = { 1200, 1600, 2000, 2400, 2800, 3200 };

    for (uint32_t i = 0; i < AUDIO_SAMPLES_PER_USB_FRAME; i++) {
        uint32_t sample_index = diag_frame_counter * AUDIO_SAMPLES_PER_USB_FRAME + i;
        for (uint32_t ch = 0; ch < AUDIO_N_CHANNELS; ch++) {
            int16_t v = ((sample_index % periods[ch]) < (periods[ch] / 2)) ? amps[ch] : -amps[ch];
            out[i * AUDIO_N_CHANNELS + ch] = master_mute ? 0 : v;
        }
    }
    diag_frame_counter++;
}
#endif

static inline void usb_audio_feed_one_frame(void) {
#if HET68_USB_DIAG
    build_diag_frame();
#else
    if (i2s_frame_ready) {
        build_usb_frame_from_i2s();
        i2s_frame_ready = false;
    } else {
        memset(usb_frame_buf, 0, sizeof(usb_frame_buf));
    }
#endif
    if (tud_audio_write(usb_frame_buf, sizeof(usb_frame_buf)) > 0) {
        dbg_usb_frames++;
    }
}

// Push one 1 ms USB frame from the main loop (not from USB callbacks).
static void audio_task(void) {
    static absolute_time_t next_ms = {0};
    if (!tud_audio_mounted() || dbg_last_alt == 0) {
        next_ms = get_absolute_time();
        return;
    }
    if (absolute_time_diff_us(get_absolute_time(), next_ms) > 0) {
        return;
    }
    next_ms = delayed_by_us(next_ms, 1000);
    usb_audio_feed_one_frame();
}

// ---------------------------------------------------------------------------
// TinyUSB UAC2 callbacks
// ---------------------------------------------------------------------------
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport,
                                   uint8_t func_id,
                                   uint8_t ep_in,
                                   uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    // Data is fed from audio_task() in the main loop (TinyUSB mic example pattern).
    return true;
}

bool tud_audio_tx_done_post_load_cb(uint8_t rhport,
                                    uint16_t n_bytes_copied,
                                    uint8_t func_id,
                                    uint8_t ep_in,
                                    uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_copied;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t itf = tu_u16_low(p_request->wIndex);
    uint8_t alt = tu_u16_low(p_request->wValue);
    dbg_last_alt = alt;
    dbg_set_itf++;
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;
    return true;
}

#ifndef tud_audio_set_itf_close_EP_cb
bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    return tud_audio_set_itf_close_ep_cb(rhport, p_request);
}
#endif

bool tud_audio_set_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *p_request,
                             uint8_t *pBuff)
{
    (void)rhport;
    (void)p_request;
    (void)pBuff;
    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;
    return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request,
                              uint8_t *pBuff)
{
    (void)rhport;
    (void)p_request;
    (void)pBuff;
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request)
{
    uint8_t channelNum = tu_u16_low(p_request->wValue);
    uint8_t ctrlSel = tu_u16_high(p_request->wValue);
    uint8_t entityID = tu_u16_high(p_request->wIndex);
    (void)channelNum;

    if (entityID == ID_IT && ctrlSel == AUDIO_TE_CTRL_CONNECTOR) {
        dbg_ctrl_other++;
        static audio_desc_channel_cluster_t connector = {
            .bNrChannels = AUDIO_N_CHANNELS,
            .bmChannelConfig = (audio_channel_config_t)(
                AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT |
                AUDIO_CHANNEL_CONFIG_FRONT_CENTER | AUDIO_CHANNEL_CONFIG_LOW_FRQ_EFFECTS |
                AUDIO_CHANNEL_CONFIG_BACK_LEFT | AUDIO_CHANNEL_CONFIG_BACK_RIGHT),
            .iChannelNames = 0
        };
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport, p_request, &connector, sizeof(connector));
    }

    if (entityID == ID_CLK && ctrlSel == AUDIO_CS_CTRL_SAM_FREQ) {
        if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
            dbg_ctrl_sam_freq++;
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, p_request, &current_sample_rate, sizeof(current_sample_rate));
        }
        if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
            dbg_ctrl_sam_freq++;
            static audio_control_range_4_n_t(1) rangef = {
                .wNumSubRanges = 1,
                .subrange[0] = {
                    .bMin = AUDIO_SAMPLE_RATE,
                    .bMax = AUDIO_SAMPLE_RATE,
                    .bRes = 0
                }
            };
            return tud_control_xfer(rhport, p_request, &rangef, sizeof(rangef));
        }
    }

    if (entityID == ID_CLK && ctrlSel == AUDIO_CS_CTRL_CLK_VALID) {
        if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
            dbg_ctrl_clk_valid++;
            return tud_control_xfer(rhport, p_request, &clock_valid, sizeof(clock_valid));
        }
    }

    if (entityID == ID_FU && ctrlSel == AUDIO_FU_CTRL_MUTE) {
        if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
            dbg_ctrl_other++;
            return tud_control_xfer(rhport, p_request, &master_mute, sizeof(master_mute));
        }
    }

    dbg_ctrl_other++;
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request,
                                 uint8_t *pBuff)
{
    (void)rhport;
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == ID_CLK &&
        request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ &&
        request->bRequest == AUDIO_CS_REQ_CUR)
    {
        if (p_request->wLength != sizeof(audio_control_cur_4_t)) return false;
        audio_control_cur_4_t const *curf = (audio_control_cur_4_t const *)pBuff;
        if ((uint32_t)curf->bCur != AUDIO_SAMPLE_RATE) return false;
        current_sample_rate = (uint32_t)curf->bCur;
        return true;
    }

    if (request->bEntityID == ID_FU &&
        request->bControlSelector == AUDIO_FU_CTRL_MUTE &&
        request->bRequest == AUDIO_CS_REQ_CUR)
    {
        if (p_request->wLength != sizeof(audio_control_cur_1_t)) return false;
        audio_control_cur_1_t const *cur_mute = (audio_control_cur_1_t const *)pBuff;
        master_mute = cur_mute->bCur ? 1u : 0u;
        return true;
    }

    return false;
}

static void dbg_heartbeat(uint32_t hb_count)
{
    dbg_puts("[");
    dbg_putu32((uint32_t)(time_us_64() / 1000000u));
    dbg_puts("s] hb=");
    dbg_putu32(hb_count);
    dbg_puts(" mnt=");
    dbg_putu32(tud_audio_mounted() ? 1u : 0u);
    dbg_puts(" alt=");
    dbg_puthex8(dbg_last_alt);
#if !HET68_USB_DIAG
    dbg_puts(" i2s=");
    dbg_putu32(i2s_frame_ready ? 1u : 0u);
#endif
    dbg_puts(" clk=");
    dbg_putu32(dbg_ctrl_clk_valid);
    dbg_puts(" freq=");
    dbg_putu32(dbg_ctrl_sam_freq);
    dbg_puts(" itf=");
    dbg_putu32(dbg_set_itf);
    dbg_puts(" usb=");
    dbg_putu32(dbg_usb_frames);
    dbg_putc('\n');
}

void tud_mount_cb(void)
{
    dbg_puts("USB mounted\n");
}

void tud_umount_cb(void)
{
    dbg_puts("USB unmounted\n");
}

// ---------------------------------------------------------------------------
int main(void)
{
    dbg_init();
    tusb_init();

    dbg_puts("\n=== het68 UAC2 6ch ===\n");
    dbg_puts("build ");
    dbg_puts(__DATE__);
    dbg_putc(' ');
    dbg_puts(__TIME__);
    dbg_putc('\n');
    dbg_puts("debug: USB CDC on Pico device (picocom /dev/ttyACM*)\n");
    dbg_puts("       or UART GP16 -> Debug Probe UTX\n");

#if !HET68_USB_DIAG
    dbg_puts("mode: I2S 3x stereo\n");
#else
    dbg_puts("mode: USB diag tones\n");
#endif

    irq_set_priority(DMA_IRQ_0, 0x80);
    irq_set_priority(USBCTRL_IRQ, 0x40);

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    bool led_state = false;
    absolute_time_t next_led = make_timeout_time_ms(500);
    absolute_time_t next_heartbeat = make_timeout_time_ms(2000);
    uint32_t hb_count = 0;

    for (;;) {
        tud_task();

#if !HET68_USB_DIAG
        if (tud_audio_mounted() && !i2s_started) {
            i2s_capture_init(AUDIO_SAMPLE_RATE);
            i2s_started = true;
            dbg_puts("I2S started\n");
        }
#endif
        audio_task();

        uint32_t blink_ms = tud_audio_mounted() ? 100u : 500u;
        if (absolute_time_diff_us(get_absolute_time(), next_led) <= 0) {
            led_state = !led_state;
            gpio_put(led_pin, led_state);
            next_led = make_timeout_time_ms(blink_ms);
        }

        if (absolute_time_diff_us(get_absolute_time(), next_heartbeat) <= 0) {
            hb_count++;
            dbg_heartbeat(hb_count);
            next_heartbeat = make_timeout_time_ms(2000);
        }
    }
}
