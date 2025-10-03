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

// ---------- Config descriptor (UAC2, 6ch mic @ 16 kHz/16-bit, ISO IN) ----------

enum { ITF_NUM_AC = 0, ITF_NUM_AS, ITF_NUM_TOTAL };

#define EPNUM_AUDIO_IN        0x01
#define EP_ADDR_AUDIO_IN      (0x80 | EPNUM_AUDIO_IN)

#define AUDIO_N_CHANNELS      6
#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_SAMPLE_BYTES    2
#define AUDIO_PACKET_SIZE     ((AUDIO_SAMPLE_RATE/1000) * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES) // 192

// Entity IDs
#define ID_CLK   0x01
#define ID_IT    0x02
#define ID_FU    0x03
#define ID_OT    0x04

static uint8_t const desc_configuration[] = {
  // ---- Standard Configuration ----
  9, TUSB_DESC_CONFIGURATION,
  0x00, 0x00,                // wTotalLength (doplníme v CB)
  ITF_NUM_TOTAL,             // bNumInterfaces
  1, 0, 0x80, 50,            // bConfigurationValue, iConfiguration, bmAttributes, bMaxPower

  // ---- IAD (Audio Function) ----
  8, TUSB_DESC_INTERFACE_ASSOCIATION,
  ITF_NUM_AC, 2,             // first IF, count (AC+AS)
  TUSB_CLASS_AUDIO, 0x00, 0x00,
  0,

  // ---- Standard AC Interface (alt 0, no EP) ----
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AC, 0, 0,
  TUSB_CLASS_AUDIO, 0x01, 0x20,   // AUDIO / AUDIOCONTROL / UAC2
  0,

  // ---- Class-Specific AC Header (UAC2) ----
  9, 0x24, 0x01, 0x00, 0x02,      // CS_INTERFACE, HEADER, bcdADC=0x0200
  0x00, 0x00,                     // wTotalLength (0 = necháme na hostu)
  1, ITF_NUM_AS,                  // bInCollection, baInterfaceNr(AS)

  // ---- Clock Source Entity ----
  8, 0x24, 0x0A, ID_CLK, 0x03, 0x07, 0x00, 0x00,

  // ---- Input Terminal (Microphone) ----
  17, 0x24, 0x02, ID_IT, 0x01, 0x02, 0x00, ID_CLK,
  AUDIO_N_CHANNELS, 0,0,0,0,      // bNrChannels=6, bmChannelConfig=0
  0x00, 0x00,                     // bmControls
  0x00,                           // iTerminal

  // ---- Feature Unit (minimal) ----
  12, 0x24, 0x06, ID_FU, ID_IT,
  0x0F, 0x00, 0x00, 0x00,         // bmaControls(0) master
  0x00, 0x00, 0x00, 0x00,         // padding / no per-channel
  // (bez iFeature)

  // ---- Output Terminal (to USB Streaming) ----
  12, 0x24, 0x03, ID_OT, 0x01, 0x01, 0x00, ID_FU, ID_CLK, 0x00, 0x00, 0x00,

  // ---- AS Interface, alt 0 (zero bandwidth) ----
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AS, 0, 0,
  TUSB_CLASS_AUDIO, 0x02, 0x20, 0,

  // ---- AS Interface, alt 1 (operational) ----
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AS, 1, 1,               // 1 isochronous IN EP
  TUSB_CLASS_AUDIO, 0x02, 0x20, 0,

  // ---- CS AS General ----
  16, 0x24, 0x01, ID_OT, 0x00, 0x01,
  0x01, 0x00, 0x00, 0x00,         // PCM
  AUDIO_N_CHANNELS,
  0,0,0,0,                        // channel config none
  0,

  // ---- Type I Format ----
  6, 0x24, 0x02, 0x01, AUDIO_SAMPLE_BYTES, 16,

  // ---- Standard ISO IN Endpoint ----
  7, TUSB_DESC_ENDPOINT,
  EP_ADDR_AUDIO_IN,
  0x0D,                           // Isochronous, Adaptive, Data
  (AUDIO_PACKET_SIZE & 0xFF), (AUDIO_PACKET_SIZE >> 8),
  0x01,                           // 1 ms

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
  "Pico 6ch Microphone",          // 2: Product
  "123456",                       // 3: Serial
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
