#include "tusb.h"
#include "tusb_config.h"
#include <string.h>

// ---------- Device descriptor (IAD at device level, matches TinyUSB audio examples) ----------
tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = 0xCafe,
  .idProduct          = 0x4066,
  .bcdDevice          = 0x0100,
  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,
  .bNumConfigurations = 0x01
};

uint8_t const* tud_descriptor_device_cb(void) {
  return (uint8_t const*) &desc_device;
}

// ---------- Config descriptor (UAC2, 6ch mic @ 48 kHz / 16-bit) ----------
enum { ITF_NUM_AC = 0, ITF_NUM_AS = 1, ITF_NUM_CDC = 2, ITF_NUM_TOTAL = 4 };
#define EPNUM_AUDIO_IN      0x01
#define EP_ADDR_AUDIO_IN    (0x80 | EPNUM_AUDIO_IN)
#define EPNUM_CDC_NOTIF     0x83
#define EPNUM_CDC_OUT       0x04
#define EPNUM_CDC_IN        0x84

#define AUDIO_N_CHANNELS    6
#define AUDIO_SAMPLE_BYTES  2
#define AUDIO_EP_SIZE       ((48000 / 1000) * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES)

// Entity IDs follow TinyUSB TUD_AUDIO_MIC_* layout: IT=1, FU=2, OT=3, CLK=4
#define ID_IT   0x01
#define ID_FU   0x02
#define ID_OT   0x03
#define ID_CLK  0x04

#define _FU_NO_CTRL 0u

#define TUD_AUDIO_DESC_FEATURE_UNIT_SIX_CHANNEL_LEN (6 + (6 + 1) * 4)
#define TUD_AUDIO_DESC_FEATURE_UNIT_SIX_CHANNEL(_unitid, _srcid, _ctrlch0master, \
    _ctrlch1, _ctrlch2, _ctrlch3, _ctrlch4, _ctrlch5, _ctrlch6, _stridx) \
  TUD_AUDIO_DESC_FEATURE_UNIT_SIX_CHANNEL_LEN, TUSB_DESC_CS_INTERFACE, \
  AUDIO_CS_AC_INTERFACE_FEATURE_UNIT, _unitid, _srcid, \
  U32_TO_U8S_LE(_ctrlch0master), U32_TO_U8S_LE(_ctrlch1), U32_TO_U8S_LE(_ctrlch2), \
  U32_TO_U8S_LE(_ctrlch3), U32_TO_U8S_LE(_ctrlch4), U32_TO_U8S_LE(_ctrlch5), \
  U32_TO_U8S_LE(_ctrlch6), _stridx

#define TUD_AUDIO_MIC_SIX_CH_DESC_LEN ( \
  TUD_AUDIO_DESC_IAD_LEN \
  + TUD_AUDIO_DESC_STD_AC_LEN \
  + TUD_AUDIO_DESC_CS_AC_LEN \
  + TUD_AUDIO_DESC_CLK_SRC_LEN \
  + TUD_AUDIO_DESC_INPUT_TERM_LEN \
  + TUD_AUDIO_DESC_OUTPUT_TERM_LEN \
  + TUD_AUDIO_DESC_FEATURE_UNIT_SIX_CHANNEL_LEN \
  + TUD_AUDIO_DESC_STD_AS_INT_LEN \
  + TUD_AUDIO_DESC_STD_AS_INT_LEN \
  + TUD_AUDIO_DESC_CS_AS_INT_LEN \
  + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN \
  + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN \
  + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)

#define TUD_AUDIO_MIC_SIX_CH_DESCRIPTOR(_itfnum, _stridx, _nBytesPerSample, _nBitsUsedPerSample, _epin, _epsize) \
  TUD_AUDIO_DESC_IAD(_itfnum, 0x02, 0x00), \
  TUD_AUDIO_DESC_STD_AC(_itfnum, 0x00, _stridx), \
  TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_MICROPHONE, \
    TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO_DESC_OUTPUT_TERM_LEN + TUD_AUDIO_DESC_FEATURE_UNIT_SIX_CHANNEL_LEN, \
    AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS), \
  TUD_AUDIO_DESC_CLK_SRC(ID_CLK, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK, \
    (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS) \
    | (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_VAL_POS), \
    ID_IT, 0x00), \
  TUD_AUDIO_DESC_INPUT_TERM(ID_IT, AUDIO_TERM_TYPE_IN_GENERIC_MIC, ID_OT, ID_CLK, \
    AUDIO_N_CHANNELS, \
    (audio_channel_config_t)(AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT | \
      AUDIO_CHANNEL_CONFIG_FRONT_CENTER | AUDIO_CHANNEL_CONFIG_LOW_FRQ_EFFECTS | \
      AUDIO_CHANNEL_CONFIG_BACK_LEFT | AUDIO_CHANNEL_CONFIG_BACK_RIGHT), \
    0x00, 0x00, 0x00), \
  TUD_AUDIO_DESC_OUTPUT_TERM(ID_OT, AUDIO_TERM_TYPE_USB_STREAMING, ID_IT, ID_FU, ID_CLK, 0x0000, 0x00), \
  TUD_AUDIO_DESC_FEATURE_UNIT_SIX_CHANNEL(ID_FU, ID_IT, \
    _FU_NO_CTRL, _FU_NO_CTRL, _FU_NO_CTRL, _FU_NO_CTRL, _FU_NO_CTRL, _FU_NO_CTRL, _FU_NO_CTRL, 0x00), \
  TUD_AUDIO_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), 0x00, 0x00, 0x00), \
  TUD_AUDIO_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), 0x01, 0x01, 0x00), \
  TUD_AUDIO_DESC_CS_AS_INT(ID_OT, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, \
    AUDIO_N_CHANNELS, \
    (audio_channel_config_t)(AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT | \
      AUDIO_CHANNEL_CONFIG_FRONT_CENTER | AUDIO_CHANNEL_CONFIG_LOW_FRQ_EFFECTS | \
      AUDIO_CHANNEL_CONFIG_BACK_LEFT | AUDIO_CHANNEL_CONFIG_BACK_RIGHT), \
    0x00), \
  TUD_AUDIO_DESC_TYPE_I_FORMAT(_nBytesPerSample, _nBitsUsedPerSample), \
  TUD_AUDIO_DESC_STD_AS_ISO_EP(_epin, \
    (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), \
    _epsize, 0x01), \
  TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE, \
    AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_MIC_SIX_CH_DESC_LEN + TUD_CDC_DESC_LEN)

// Export for tusb_config.h consistency check in build scripts if needed.
enum { HET68_AUDIO_FUNC_DESC_LEN = TUD_AUDIO_MIC_SIX_CH_DESC_LEN };

uint8_t const desc_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),
  TUD_AUDIO_MIC_SIX_CH_DESCRIPTOR(ITF_NUM_AC, 0, AUDIO_SAMPLE_BYTES, 16, EP_ADDR_AUDIO_IN, AUDIO_EP_SIZE),
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_configuration;
}

// ---------- String descriptors ----------
static char const* string_desc[] = {
  (const char[]){ 0x09, 0x04 },
  "het68",
  "Pico 6ch Microphone 48k/16",
  "123654",
  "het68 debug",
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  uint8_t chr_count;
  if (index == 0) {
    memcpy(&_desc_str[1], string_desc[0], 2);
    chr_count = 1;
  } else {
    if (index >= (sizeof(string_desc)/sizeof(string_desc[0]))) return NULL;
    const char* str = string_desc[index];
    chr_count = (uint8_t)strlen(str);
    if (chr_count > 31) chr_count = 31;
    for (uint8_t i = 0; i < chr_count; i++) _desc_str[1+i] = str[i];
  }
  _desc_str[0] = (TUSB_DESC_STRING << 8) | (2*chr_count + 2);
  return _desc_str;
}
