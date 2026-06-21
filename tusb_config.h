#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// --- MCU & OS ---
// Pico 2 / RP2350 with a recent pico-sdk defines PICO_RP2350.
// Older builds configured for RP2040 fall back to the RP2040 MCU option.
#ifndef CFG_TUSB_MCU
  #if defined(PICO_RP2350) && defined(OPT_MCU_RP2350)
    #define CFG_TUSB_MCU    OPT_MCU_RP2350
  #else
    #define CFG_TUSB_MCU    OPT_MCU_RP2040
  #endif
#endif

#define CFG_TUSB_OS     OPT_OS_PICO

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG  0
#endif

// --- COMMON ---
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE      64

// --- ENABLED CLASSES ---
#define CFG_TUD_CDC     1
#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  256
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUD_MSC     0
#define CFG_TUD_HID     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_VENDOR  0
#define CFG_TUD_AUDIO   1

// --- AUDIO (6ch mic: device -> host @ 48kHz/24-bit, packed S24_3LE) ---
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ            64
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN               152
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT               1

#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX          6
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX          0

#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX  3
#define CFG_TUD_AUDIO_FUNC_1_N_BITS_PER_SAMPLE_TX   24
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE        48000

// 1ms frame: 48 kHz / 1000 * 6ch * 3 B = 864 B (< 1023 B FS iso limit)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX           (48000 / 1000 * 6 * 3)

// Was 2x packet originally; widened to 8x for a more stable isochronous IN stream.
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ        (8 * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)

#ifndef CFG_TUD_AUDIO_ENABLE_EP_IN
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#endif

#ifndef CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 0
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
