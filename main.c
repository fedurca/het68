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
#include "tusb.h"

// Custom panic handler registered via PICO_PANIC_FUNCTION=het68_panic (CMakeLists.txt).
// Without this, pico-sdk's panic() calls __breakpoint() which halts the CPU before
// the UART FIFO has drained, silently swallowing the panic reason when the Debug Probe
// is connected.
void __attribute__((noreturn)) het68_panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    puts("\n!!! PANIC !!!\n");
    vprintf(fmt, args);
    puts("\n");
    va_end(args);
    sleep_ms(300); // let UART FIFO drain at 115200 baud (~200 chars @ 115200)
    for (;;) { __asm volatile("nop"); }  // spin — no __breakpoint so UART stays alive
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
    (void)rhport;
    (void)p_request;
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

int main(void)
{
    stdio_init_all();
    printf("\n\n=== het68 UAC2 6ch diagnostic firmware starting ===\n");
    printf("    AUDIO: %d ch, %d Hz, %d bit\n",
           AUDIO_N_CHANNELS, AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_BYTES * 8);
    printf("    PACKET: %d bytes/frame\n", AUDIO_PACKET_SIZE);
    tusb_init();
    printf("    TinyUSB init OK\n");

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    bool led_state = false;
    absolute_time_t next_heartbeat = make_timeout_time_ms(100);

    for (;;) {
        tud_task();

        // Fallback pre-fill path for TinyUSB variants where the TX callbacks are
        // only invoked after the first successful transfer. This is intentionally
        // conservative: one frame per main-loop tick when mounted.
        if (tud_audio_mounted()) {
            usb_audio_feed_one_frame();
        }

        if (absolute_time_diff_us(get_absolute_time(), next_heartbeat) <= 0) {
            led_state = !led_state;
            gpio_put(led_pin, led_state);
            next_heartbeat = make_timeout_time_ms(100);
        }
    }
}
