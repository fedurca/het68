#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// --- MCU & OS ---
// Pico 2 / RP2350 s novým pico-sdk definuje PICO_RP2350.
// Pokud používáš starší build nastavený pro RP2040, zůstane fallback RP2040.
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
#define CFG_TUD_CDC     0
#define CFG_TUD_MSC     0
#define CFG_TUD_HID     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_VENDOR  0
#define CFG_TUD_AUDIO   1

// --- AUDIO (6ch mic: device -> host @ 48kHz/16-bit) ---
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ            64
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN               152
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT               1

#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX          6
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX          0

#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX  2
#define CFG_TUD_AUDIO_FUNC_1_N_BITS_PER_SAMPLE_TX   16
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE        48000

// 1ms frame: 48 kHz / 1000 * 6ch * 2 B = 576 B
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX           (48000 / 1000 * 6 * 2)

// Původně bylo 2× packet. Pro stabilnější isochronní IN stream zvětšeno na 8×.
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
