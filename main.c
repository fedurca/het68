// main.c — RP2350 UAC2 6ch microphone (3× ICS-43434 stereo pairs via PIO I2S RX + DMA)
//
// Pinout (see README.md / wiring_and_bom.md):
//   GP0 = WS/LRCLK, GP1 = BCLK/SCK (Pico is I2S master)
//   GP2 = SD pair 1+2, GP3 = SD pair 3+4, GP4 = SD pair 5+6
//   GP8 = UART TX (yellow, pin 11), GP9 = UART RX (orange, pin 12), GND (black, pin 13)
//
// HET68_USB_DIAG=1 bypasses I2S and sends a simulated 1 kHz tone (bench test).

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
#include "i2s_clk.pio.h"

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
    // Bounded: never hang here even if the UART FIFO is wedged (e.g. no probe).
    for (int spin = 0; spin < 100000 && !uart_is_writable(uart_default); spin++) { }
    for (volatile int i = 0; i < 12000; i++) { }
}

// Emergency post-mortem "beacon": after a panic/hardfault we can no longer rely
// on the main loop, so keep USB/CDC alive here ourselves and re-emit the reason
// (and fault PC/LR) on UART *and* USB CDC forever. This lets a host attach to
// the Pico CDC (/dev/ttyACM1) AFTER the crash and still read why it died — vital
// when the probe UART is unavailable. Never returns.
static void __attribute__((noreturn)) het68_beacon(const char *reason,
                                                    uint32_t pc, uint32_t lr) {
    bool have_regs = (pc != 0 || lr != 0);
    for (;;) {
        raw_puts("\n!!! ");
        raw_puts(reason);
        raw_puts(" !!! up=");
        raw_putu32((uint32_t)(time_us_64() / 1000000));
        raw_puts("s");
        if (have_regs) {
            raw_puts(" PC=");
            raw_puthex32(pc);
            raw_puts(" LR=");
            raw_puthex32(lr);
        }
        raw_puts("\n");
        raw_flush();
#if HET68_DEBUG_CDC && CFG_TUD_CDC
        // Mirror to CDC and keep the device alive so a late reader still sees it.
        dbg_puts("\n!!! ");
        dbg_puts(reason);
        dbg_puts(" !!! PANIC-BEACON\n");
        for (int i = 0; i < 200; i++) {
            tud_task();
            tud_cdc_write_flush();
            sleep_ms(2);
        }
#else
        sleep_ms(400);
#endif
    }
}

void __attribute__((noreturn)) het68_panic(const char *fmt, ...) {
    (void)fmt;
    het68_beacon("PANIC", 0, 0);
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
    // frame[6]=PC, frame[5]=LR (Cortex-M exception stack frame).
    het68_beacon("HARDFAULT", frame[6], frame[5]);
}

void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
    (void)fmt;
    het68_beacon("PANIC(wrap)", 0, 0);
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
#define I2S_CLK_SM              3u
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

// I2S/DMA diagnostics (heartbeat + bring-up).
static volatile uint32_t dbg_i2s_dma_irq;
static volatile uint32_t dbg_i2s_feed_ok;
static volatile uint32_t dbg_i2s_feed_miss;
static volatile uint16_t dbg_i2s_peak[6];
static volatile uint32_t dbg_i2s_raw[3];

static void i2s_clk_sm_init(uint offset, float div) {
    pio_gpio_init(i2s_pio, PIN_I2S_WS);
    pio_gpio_init(i2s_pio, PIN_I2S_SCK);
    pio_sm_set_consecutive_pindirs(i2s_pio, I2S_CLK_SM, PIN_I2S_WS, 1, true);
    pio_sm_set_consecutive_pindirs(i2s_pio, I2S_CLK_SM, PIN_I2S_SCK, 1, true);

    pio_sm_config c = i2s_clk_program_get_default_config(offset);
    sm_config_set_set_pins(&c, PIN_I2S_WS, 1);
    sm_config_set_sideset_pins(&c, PIN_I2S_SCK);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(i2s_pio, I2S_CLK_SM, offset, &c);
}

static void i2s_data_sm_init(uint sm, uint offset, uint pin_data, float div) {
    pio_gpio_init(i2s_pio, pin_data);
    pio_sm_set_consecutive_pindirs(i2s_pio, sm, pin_data, 1, false);

    pio_sm_config c = i2s_rx_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin_data);
    // MSB-first: shift LEFT so the first received bit (sample MSB) lands in the
    // ISR MSB; autopush a full 32-bit slot per channel.
    sm_config_set_in_shift(&c, false, true, 32);
    // CRITICAL: identical clkdiv to the clock SM so the two stay phase-locked
    // when started together with pio_enable_sm_mask_in_sync(). The RX program
    // mirrors the clock SM cycle-for-cycle and samples deterministically.
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
    dbg_i2s_dma_irq++;
    i2s_ready_half = (uint8_t)(1u - i2s_ready_half);
    i2s_frame_ready = true;
    i2s_dma_start_half(i2s_ready_half);
}

static void i2s_capture_init(uint32_t sample_rate) {
    i2s_pio = pio0;
    uint offset_clk = pio_add_program(i2s_pio, &i2s_clk_program);
    uint offset_rx = pio_add_program(i2s_pio, &i2s_rx_program);

    // 64 BCLK per frame (32/channel), 3 SM cycles per bit, 2 setup instr per
    // half → frame = 2*(2 + 32*3) = 196 SM cycles. Pick clkdiv so one frame
    // takes 1/sample_rate. The clock SM and all data SMs use this SAME div so
    // pio_enable_sm_mask_in_sync() leaves them phase-locked (deterministic RX).
    float div = (float)clock_get_hz(clk_sys) / (196.0f * (float)sample_rate);
    i2s_clk_sm_init(offset_clk, div);

    const uint pin_data[I2S_NUM_LINES] = { PIN_I2S_D01, PIN_I2S_D23, PIN_I2S_D45 };

    for (uint i = 0; i < I2S_NUM_LINES; i++) {
        i2s_sm[i] = i;
        i2s_data_sm_init(i2s_sm[i], offset_rx, pin_data[i], div);

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

    uint32_t mask = (1u << I2S_CLK_SM);
    for (uint i = 0; i < I2S_NUM_LINES; i++) mask |= (1u << i2s_sm[i]);
    pio_enable_sm_mask_in_sync(i2s_pio, mask);

    dbg_puts("I2S started clk_sm=");
    dbg_putu32(I2S_CLK_SM);
    dbg_puts(" dma0=");
    dbg_putu32((uint32_t)i2s_dma[0]);
    dbg_putc('\n');
}

// The RX SM captures 32 bits MSB-first per WS slot. Where the sample MSB lands
// in that word was measured empirically against a 1 kHz lab reference (clean,
// symmetric, low-THD only at these shifts):
//   - RIGHT slot (WS=1): standard I2S 1-bit delay -> MSB at bit30 -> shift left 1
//   - LEFT  slot (WS=0): arrives 3 BCLK later     -> MSB at bit27 -> shift left 4
// After the left shift the MSB sits in bit31; an arithmetic >>8 then sign-extends
// to a 24-bit value in the low 24 bits.
#define I2S_LSHIFT_LEFT   4u
#define I2S_LSHIFT_RIGHT  1u
static inline int32_t i2s_word_to_s24(uint32_t raw, uint lshift) {
    return ((int32_t)(raw << lshift)) >> 8;
}

static void build_usb_frame_from_i2s(void) {
    uint8_t read_half = (uint8_t)(1u - i2s_ready_half);
    uint32_t base = (uint32_t)read_half * I2S_WORDS_PER_FRAME;

    uint8_t *out = usb_frame_buf;
    uint16_t peak[6] = {0, 0, 0, 0, 0, 0};
    uint32_t raw_at_peak[3] = {0, 0, 0};
    uint16_t line_peak[3] = {0, 0, 0};
    for (uint32_t s = 0; s < AUDIO_SAMPLES_PER_USB_FRAME; s++) {
        uint32_t w = base + s * 2u;
        uint32_t raww[6] = {
            i2s_cap[0][w], i2s_cap[0][w + 1u],
            i2s_cap[1][w], i2s_cap[1][w + 1u],
            i2s_cap[2][w], i2s_cap[2][w + 1u],
        };

        for (int i = 0; i < 6; i++) {
            // Even index = LEFT slot (WS=0), odd = RIGHT slot (WS=1).
            uint lshift = (i & 1) ? I2S_LSHIFT_RIGHT : I2S_LSHIFT_LEFT;
            int32_t s24 = i2s_word_to_s24(raww[i], lshift);
            int32_t v = s24 < 0 ? -s24 : s24;
            uint16_t a = (uint16_t)(v >> 8);   // track top 16 bits for the heartbeat
            if (a > peak[i]) peak[i] = a;
            // Remember the raw 32-bit word at the loudest sample of each line.
            if (a > line_peak[i >> 1]) { line_peak[i >> 1] = a; raw_at_peak[i >> 1] = raww[i]; }
            if (master_mute) s24 = 0;
            // Pack signed 24-bit little-endian (S24_3LE).
            *out++ = (uint8_t)(s24 & 0xFF);
            *out++ = (uint8_t)((s24 >> 8) & 0xFF);
            *out++ = (uint8_t)((s24 >> 16) & 0xFF);
        }
    }
    for (int i = 0; i < 6; i++) dbg_i2s_peak[i] = peak[i];
    dbg_i2s_raw[0] = raw_at_peak[0];
    dbg_i2s_raw[1] = raw_at_peak[1];
    dbg_i2s_raw[2] = raw_at_peak[2];
}
#endif  // !HET68_USB_DIAG

#if HET68_USB_DIAG
#include <math.h>

// Simulated constant tone (mics are physically disconnected). One USB frame holds
// exactly AUDIO_SAMPLES_PER_USB_FRAME = AUDIO_SAMPLE_RATE/1000 samples, so a LUT of
// one full sine period per frame yields a steady 1 kHz tone at any sample rate.
// Each frame is identical, so no phase accumulator is needed.
#define DIAG_TONE_AMPLITUDE  8000
static int16_t diag_tone_lut[AUDIO_SAMPLES_PER_USB_FRAME];
static bool    diag_lut_ready;

static void diag_build_lut(void) {
    for (uint32_t i = 0; i < AUDIO_SAMPLES_PER_USB_FRAME; i++) {
        double theta = 2.0 * 3.14159265358979323846 *
                       (double)i / (double)AUDIO_SAMPLES_PER_USB_FRAME;
        diag_tone_lut[i] = (int16_t)((double)DIAG_TONE_AMPLITUDE * sin(theta));
    }
    diag_lut_ready = true;
}

static void build_diag_frame(void) {
    if (!diag_lut_ready) diag_build_lut();
    (void)diag_frame_counter;
    diag_frame_counter++;

    int16_t *out = (int16_t *)usb_frame_buf;
    for (uint32_t i = 0; i < AUDIO_SAMPLES_PER_USB_FRAME; i++) {
        int16_t v = master_mute ? 0 : diag_tone_lut[i];
        for (uint32_t ch = 0; ch < AUDIO_N_CHANNELS; ch++) {
            out[i * AUDIO_N_CHANNELS + ch] = v;
        }
    }
}
#endif

// Build exactly one 1 ms USB audio packet and hand it to the TinyUSB EP IN FIFO.
//
// This is called from tud_audio_tx_done_pre_load_cb() (see below), i.e. from the
// USB device task immediately before each isochronous IN packet is shipped. That
// is the deterministic, race-free place to produce data: the bytes written here
// are sent in the *same* audiod_tx_done_cb() invocation, so every IN packet —
// including the very first (deferred) one after SET_INTERFACE alt=1 — always
// carries a full frame. Producing data from the main loop instead would race the
// first transfer and make the host see an empty packet, which it punishes by
// dropping back to alt=0 (the cold-start "SETIF 1 → SETIF 0" churn we observed).
static inline void usb_audio_feed_one_frame(void) {
#if HET68_USB_DIAG
    build_diag_frame();
#else
    if (i2s_frame_ready) {
        dbg_i2s_feed_ok++;
        build_usb_frame_from_i2s();
        i2s_frame_ready = false;
    } else {
        dbg_i2s_feed_miss++;
        // No fresh I2S frame yet: send silence rather than stalling the endpoint.
        // A continuously-fed ISO IN stream is what keeps the host streaming.
        memset(usb_frame_buf, 0, sizeof(usb_frame_buf));
    }
#endif
    // tud_audio_write() copies into the EP IN FIFO. It returns 0 only if the FIFO
    // is unexpectedly full (we always drain exactly one packet per IN, so in
    // steady state it never is); count only frames actually queued.
    if (tud_audio_write(usb_frame_buf, sizeof(usb_frame_buf)) > 0) {
        dbg_usb_frames++;
    }
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
    // Produce the next packet right here so it ships in this same IN transfer.
    // The stack only calls this when the streaming alt setting is non-zero, so we
    // never feed while idle. This is what makes the stream start deterministically.
    usb_audio_feed_one_frame();
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
    // Non-blocking UART checkpoint: "SETIF <itf> <alt>"
    dbg_puts("SETIF ");
    dbg_puthex8(itf);
    dbg_putc(' ');
    dbg_puthex8(alt);
    dbg_putc('\n');
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

    // Non-blocking UART checkpoint: "GET <entity> <selector> <request>"
    dbg_puts("GET ");
    dbg_puthex8(entityID);
    dbg_putc(' ');
    dbg_puthex8(ctrlSel);
    dbg_putc(' ');
    dbg_puthex8(p_request->bRequest);
    dbg_putc('\n');

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
        return tud_control_xfer(rhport, p_request, &connector, sizeof(connector));
    }

    if (entityID == ID_CLK && ctrlSel == AUDIO_CS_CTRL_SAM_FREQ) {
        if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
            dbg_ctrl_sam_freq++;
            return tud_control_xfer(
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

    // Non-blocking UART checkpoint: "SET <entity> <selector> <request>"
    dbg_puts("SET ");
    dbg_puthex8(request->bEntityID);
    dbg_putc(' ');
    dbg_puthex8(request->bControlSelector);
    dbg_putc(' ');
    dbg_puthex8(request->bRequest);
    dbg_putc('\n');

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

#if !HET68_USB_DIAG
static bool i2s_sm_stalled(uint sm) {
    return (i2s_pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + sm))) != 0u;
}

static void dbg_heartbeat_i2s(void) {
    dbg_puts(" dma=");
    dbg_putu32(dbg_i2s_dma_irq);
    dbg_puts(" ok=");
    dbg_putu32(dbg_i2s_feed_ok);
    dbg_puts(" miss=");
    dbg_putu32(dbg_i2s_feed_miss);
    dbg_puts(" pk=");
    dbg_putu32((uint32_t)dbg_i2s_peak[0]);
    dbg_putc(',');
    dbg_putu32((uint32_t)dbg_i2s_peak[1]);
    dbg_putc(',');
    dbg_putu32((uint32_t)dbg_i2s_peak[2]);
    dbg_puts(" raw=");
    dbg_puthex32(dbg_i2s_raw[0]);
    dbg_putc(',');
    dbg_puthex32(dbg_i2s_raw[1]);
    dbg_putc(',');
    dbg_puthex32(dbg_i2s_raw[2]);
    dbg_puts(" stall=");
    if (i2s_started) {
        dbg_putu32(i2s_sm_stalled(0) ? 1u : 0u);
        dbg_putc('/');
        dbg_putu32(i2s_sm_stalled(I2S_CLK_SM) ? 1u : 0u);
    } else {
        dbg_puts("na");
    }
}
#endif

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
    dbg_heartbeat_i2s();
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

#if HET68_USB_DIAG
    // Precompute the tone table before USB can start streaming, so the first
    // (deferred) IN packet is already valid and no float math runs inside the
    // USB callback path.
    diag_build_lut();
#endif

    tusb_init();

    dbg_puts("\n=== het68 UAC2 6ch ===\n");
    dbg_puts("build ");
    dbg_puts(__DATE__);
    dbg_putc(' ');
    dbg_puts(__TIME__);
    dbg_putc('\n');
#if !HET68_USB_DIAG
    dbg_puts("debug: UART GP8 -> Debug Probe UART RX (pins 11/12/13)\n");
    dbg_puts("mode: I2S 3x stereo\n");
#else
    dbg_puts("debug: UART GP8 -> Debug Probe UART RX (pins 11/12/13)\n");
    dbg_puts("mode: simulated 1kHz tone (I2S bypassed)\n");
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
        }
#endif
        // Audio frames are produced in tud_audio_tx_done_pre_load_cb(), driven by
        // the USB IN cadence — nothing to pump from the main loop here.

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
