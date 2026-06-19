// main.c — TinyUSB UAC2 6ch diagnostic tone/silence generator
// Purpose: isolate USB Audio streaming from PIO/I2S/DMA.
// It enumerates as your existing 6ch / 48 kHz / 16-bit microphone and always
// feeds a valid 576-byte frame to the ISO IN endpoint.
//
// If this records without arecord I/O error, USB descriptors + TinyUSB streaming
// are basically OK and the remaining bug is in PIO/I2S/DMA capture.
// If this still fails, the remaining bug is in USB Audio descriptors or TinyUSB
// callback/API mismatch.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "tusb.h"

// ---------------------------------------------------------------------------
// Raw UART helpers — safe to call from interrupt/fault context, no mutex.
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
    char buf[11]; int n = 0;
    for (; v; v /= 10) buf[n++] = '0' + (v % 10);
    while (n--) uart_putc_raw(uart_default, buf[n]);
}

// Spin until UART TX FIFO fully drained (~10 chars at 115200 = 0.87 ms).
static void raw_flush(void) {
    while (uart_is_writable(uart_default) == false) { }
    // UART FIFO drained to hardware shift register; give one more char-time.
    for (volatile int i = 0; i < 12000; i++) { }
}

// ---------------------------------------------------------------------------
// Custom panic handler (PICO_PANIC_FUNCTION=het68_panic).
// Uses raw UART so the message is visible even from fault/ISR context.
// ---------------------------------------------------------------------------
void __attribute__((noreturn)) het68_panic(const char *fmt, ...) {
    raw_puts("\n!!! PANIC !!! uptime=");
    raw_putu32((uint32_t)(time_us_64() / 1000000));
    raw_puts("s\n");
    // best-effort formatted print (may be unavailable in deep fault context)
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    raw_puts("\n");
    raw_flush();
    for (;;) { __asm volatile("nop"); }
}

// ---------------------------------------------------------------------------
// HardFault handler — catches NULL deref, bad jump, stack overflow etc.
// Prints PC/LR from the faulting frame so we can find the crash site.
// ---------------------------------------------------------------------------
void __attribute__((naked)) isr_hardfault(void) {
    // Stacked frame (PSP or MSP): R0 R1 R2 R3 R12 LR PC xPSR
    __asm volatile (
        "tst lr, #4        \n"  // test bit 2 of EXC_RETURN
        "ite eq            \n"
        "mrseq r0, msp     \n"  // 0 -> faulted on MSP
        "mrsne r0, psp     \n"  // 1 -> faulted on PSP
        "b het68_hardfault \n"
        ::: "r0"
    );
}

// Only raw register writes — no stdlib, no timer — to prevent double-fault.
void __attribute__((noreturn)) het68_hardfault(uint32_t *frame) {
    raw_puts("\n!!! HARDFAULT !!!\n");
    raw_puts("PC="); raw_puthex32(frame[6]);
    raw_puts(" LR="); raw_puthex32(frame[5]);
    raw_puts("\n");
    raw_flush();
    for (;;) { __asm volatile("nop"); }
}

// --wrap=panic catches pre-built pico-sdk libraries that don't see het68_panic.
void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    raw_puts("\n!!! PANIC(wrap) !!!\n");
    vprintf(fmt, args);
    va_end(args);
    raw_puts("\n");
    raw_flush();
    for (;;) { __asm volatile("nop"); }
}

#define AUDIO_SAMPLE_RATE   CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
#define AUDIO_N_CHANNELS    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
#define AUDIO_SAMPLE_BYTES  CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX
#define AUDIO_SAMPLES_PER_USB_FRAME  (AUDIO_SAMPLE_RATE / 1000)  // 48
#define AUDIO_PACKET_SIZE   (AUDIO_SAMPLES_PER_USB_FRAME * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES)

#define ID_CLK  0x01
#define ID_FU   0x03

// 1 = audible/debug square-ish test signal in all 6 channels.
// 0 = pure silence.
#ifndef USB_DIAG_TONE
#define USB_DIAG_TONE 1
#endif

static uint8_t usb_frame_buf[AUDIO_PACKET_SIZE] __attribute__((aligned(4)));
static uint32_t frame_counter = 0;

static uint32_t current_sample_rate = AUDIO_SAMPLE_RATE;
static uint8_t clock_valid = 1;
static uint8_t master_mute = 0;

static void build_diag_frame(void)
{
    int16_t *out = (int16_t *)usb_frame_buf;

#if USB_DIAG_TONE
    // Different simple square-wave-ish patterns per channel. No libm needed.
    // Amplitude is intentionally low to avoid speaker/headphone surprises.
    static const uint16_t periods[6] = { 109, 97, 83, 71, 61, 53 };
    static const int16_t amps[6] = { 1200, 1600, 2000, 2400, 2800, 3200 };

    for (uint32_t i = 0; i < AUDIO_SAMPLES_PER_USB_FRAME; i++) {
        uint32_t sample_index = frame_counter * AUDIO_SAMPLES_PER_USB_FRAME + i;
        for (uint32_t ch = 0; ch < 6; ch++) {
            int16_t v = ((sample_index % periods[ch]) < (periods[ch] / 2)) ? amps[ch] : -amps[ch];
            out[i * 6u + ch] = master_mute ? 0 : v;
        }
    }
    frame_counter++;
#else
    memset(usb_frame_buf, 0, sizeof(usb_frame_buf));
#endif
}

static inline void usb_audio_feed_one_frame(void)
{
    build_diag_frame();
    (void)tud_audio_write(usb_frame_buf, sizeof(usb_frame_buf));
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport,
                                   uint8_t func_id,
                                   uint8_t ep_in,
                                   uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;

    if (cur_alt_setting != 0) {
        usb_audio_feed_one_frame();
    }
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
    uint8_t const alt = (uint8_t)(p_request->wValue & 0xffu);
    uint8_t const itf = (uint8_t)(p_request->wIndex & 0xffu);
    printf("[%5lus] SET_INTERFACE iface=%u alt=%u\n",
           (unsigned long)(time_us_64() / 1000000), itf, alt);
    dbg_pulse(alt ? 2 : 1);
    raw_puts("[CP-C] set_itf_cb returning true\n");
    raw_flush();

    // Guarantee the SET_INTERFACE status ZLP is sent now, before
    // audiod_set_interface() tries to send it (which may fail due to
    // EP0_IN busy race).  The second call from audiod_set_interface will
    // silently fail (busy=1) which is fine — the ZLP is already queued.
    tud_control_status(rhport, p_request);

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
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == ID_CLK &&
        request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
    {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t curf = { .bCur = current_sample_rate };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &curf, sizeof(curf));
        }

        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) rangef = {
                .wNumSubRanges = 1,
                .subrange[0] = {
                    .bMin = AUDIO_SAMPLE_RATE,
                    .bMax = AUDIO_SAMPLE_RATE,
                    .bRes = 0
                }
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &rangef, sizeof(rangef));
        }
    }

    if (request->bEntityID == ID_CLK &&
        request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID)
    {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_1_t cur_valid = { .bCur = clock_valid };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur_valid, sizeof(cur_valid));
        }
    }

    if (request->bEntityID == ID_FU &&
        request->bControlSelector == AUDIO_FU_CTRL_MUTE)
    {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            // All channels report the same master mute state.
            audio_control_cur_1_t cur_mute = { .bCur = master_mute };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur_mute, sizeof(cur_mute));
        }
    }

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

void dbg_pulse(uint8_t n);  // defined below main()

void dbg_pulse(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        gpio_put(2, 1); for (volatile int d=0; d<5000; d++) {}
        gpio_put(2, 0); for (volatile int d=0; d<5000; d++) {}
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(2); gpio_set_dir(2, GPIO_OUT);
    dbg_pulse(3);  // 3 pulses = firmware started

    printf("\n\n=== het68 UAC2 6ch diagnostic firmware ===\n");
    printf("    Build : %s %s\n", __DATE__, __TIME__);
    printf("    Audio : %d ch, %d Hz, %d bit, %d B/frame\n",
           AUDIO_N_CHANNELS, AUDIO_SAMPLE_RATE,
           AUDIO_SAMPLE_BYTES * 8, AUDIO_PACKET_SIZE);
    tusb_init();
    printf("    TinyUSB 0.18.0 + PR#2937+4bfba6b patches\n");
    printf("==========================================\n");

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    bool led_state = false;
    absolute_time_t next_led       = make_timeout_time_ms(500);
    absolute_time_t next_heartbeat = make_timeout_time_ms(10000);
    uint32_t hb_count = 0;

    for (;;) {
        tud_task();

        if (tud_audio_mounted()) {
            usb_audio_feed_one_frame();
        }

        uint32_t blink_ms = tud_audio_mounted() ? 100 : 500;
        if (absolute_time_diff_us(get_absolute_time(), next_led) <= 0) {
            led_state = !led_state;
            gpio_put(led_pin, led_state);
            next_led = make_timeout_time_ms(blink_ms);
        }

        if (absolute_time_diff_us(get_absolute_time(), next_heartbeat) <= 0) {
            hb_count++;
            printf("[%5lus] heartbeat #%lu  mounted=%d\n",
                   (unsigned long)(time_us_64() / 1000000),
                   (unsigned long)hb_count,
                   (int)tud_audio_mounted());
            next_heartbeat = make_timeout_time_ms(10000);
        }
    }
}
