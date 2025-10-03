#include "tusb.h"
#include <string.h>

// ---------- Device descriptor ----------
tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = 0x00,
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,
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

// ---------- Config descriptor (UAC2, 2ch mic @ 48 kHz / 16-bit packed) ----------
enum { ITF_NUM_AC = 0, ITF_NUM_AS, ITF_NUM_TOTAL };
#define EPNUM_AUDIO_IN      0x01
#define EP_ADDR_AUDIO_IN    (0x80 | EPNUM_AUDIO_IN)

// MODIFIED for 2-channel, 16-bit, 48kHz
#define AUDIO_N_CHANNELS    2
#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_SAMPLE_BYTES  2
#define AUDIO_PACKET_SIZE   ((AUDIO_SAMPLE_RATE/1000) * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES) // 48 * 2 * 2 = 192

#define ID_CLK  0x01
#define ID_IT   0x02
#define ID_FU   0x03
#define ID_OT   0x04

// MODIFIED Feature Unit length for 2 channels: 5 + (2+1)*4 + 1 = 18
#define LEN_CS_AC_HEADER    9
#define LEN_CS_CLK_SRC      8
#define LEN_CS_IT           17
#define LEN_CS_FU           18
#define LEN_CS_OT           12

// MODIFIED Total AC length: 9+8+17+18+12 = 64
#define AC_CS_TOTAL         64

static uint8_t const desc_configuration[] = {
  // ---- Standard Configuration ----
  9, TUSB_DESC_CONFIGURATION,
  0x00, 0x00, ITF_NUM_TOTAL, 1, 0, 0x80, 50,
  // ---- IAD (Audio Function) ----
  8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_AC, 2, TUSB_CLASS_AUDIO, 0x00, 0x00, 0,
  // ---- Standard AC Interface ----
  9, TUSB_DESC_INTERFACE, ITF_NUM_AC, 0, 0, TUSB_CLASS_AUDIO, 0x01, 0x20, 0,
  // ---- Class-Specific AC Header ----
  LEN_CS_AC_HEADER, 0x24, 0x01, 0x00, 0x02, (uint8_t)(AC_CS_TOTAL & 0xFF), (uint8_t)(AC_CS_TOTAL >> 8), 1, ITF_NUM_AS,
  // ---- Clock Source Entity ----
  LEN_CS_CLK_SRC, 0x24, 0x0A, ID_CLK, 0x01, 0x01, 0x00, 0x00,
  // ---- Input Terminal ----
  LEN_CS_IT, 0x24, 0x02, ID_IT, 0x01, 0x02, 0x00, ID_CLK, AUDIO_N_CHANNELS, 0,0,0,0, 0x00, 0x00, 0x00, 0x00,
  // ---- Feature Unit (MODIFIED for 2 channels)----
  LEN_CS_FU, 0x24, 0x06, ID_FU, ID_IT,
  0x03, 0x00, 0x00, 0x00, // Master controls
  0x00, 0x00, 0x00, 0x00, // Channel 1 controls
  0x00, 0x00, 0x00, 0x00, // Channel 2 controls
  0x00,                   // iFeature
  // ---- Output Terminal ----
  LEN_CS_OT, 0x24, 0x03, ID_OT, 0x01, 0x01, 0x00, ID_FU, ID_CLK, 0x00, 0x00, 0x00,
  // ---- AS Interface, alt 0 ----
  9, TUSB_DESC_INTERFACE, ITF_NUM_AS, 0, 0, TUSB_CLASS_AUDIO, 0x02, 0x20, 0,
  // ---- AS Interface, alt 1 ----
  9, TUSB_DESC_INTERFACE, ITF_NUM_AS, 1, 1, TUSB_CLASS_AUDIO, 0x02, 0x20, 0,
  // ---- CS AS General ----
  16, 0x24, 0x01, ID_OT, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, AUDIO_N_CHANNELS, 0,0,0,0, 0,
  // ---- Type I Format (MODIFIED for 16-bit) ----
  6, 0x24, 0x02, 0x01, AUDIO_SAMPLE_BYTES, 16,
  // ---- Standard ISO IN Endpoint ----
  7, TUSB_DESC_ENDPOINT, EP_ADDR_AUDIO_IN, 0x05, (AUDIO_PACKET_SIZE & 0xFF), (AUDIO_PACKET_SIZE >> 8), 0x01,
  // ---- CS ISO IN Endpoint ----
  8, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint8_t cfg_desc_buf[sizeof(desc_configuration)];

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  memcpy(cfg_desc_buf, desc_configuration, sizeof(desc_configuration));
  uint16_t const total_len = sizeof(desc_configuration);
  cfg_desc_buf[2] = (uint8_t)(total_len & 0xFF);
  cfg_desc_buf[3] = (uint8_t)(total_len >> 8);
  return cfg_desc_buf;
}

// ---------- String descriptors ----------
static char const* string_desc[] = {
  (const char[]){ 0x09, 0x04 },   // 0: English (US)
  "het68",                        // 1: Manufacturer
  "Pico 2ch Microphone 48k/16",   // 2: Product (MODIFIED)
  "123456",                       // 3: Serial
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) index;
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
