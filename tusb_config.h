#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE      64

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               1

//--------------------------------------------------------------------
// AUDIO CONFIGURATION
//--------------------------------------------------------------------
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN           TUD_AUDIO_DESC_IAD_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT           1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX      0
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX      6 // Počet kanálů pro nahrávání (IN pro hosta)
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX 2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX 2 // 16-bit
#define CFG_TUD_AUDIO_FUNC_1_N_BITS_PER_SAMPLE_TX  16
#define CFG_TUD_AUDIO_FUNC_1_N_BITS_PER_SAMPLE_RX  16

#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE    16000 // Musí odpovídat konfiguraci

#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    (16000 / 1000 * 6 * 2 * 2) // Buffer na 2ms
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       (16000 / 1000 * 6 * 2)     // Paket na 1ms


#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */

