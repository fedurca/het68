#include "tusb.h"
#include <string.h>

// ---------------- Device descriptor ----------------
tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = 0x00,
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE, // = 64 for FS
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

// ---------------- Audio UAC2: 6ch @ 48k/24b (packed) ----------------
enum { ITF_NUM_AC = 0, ITF_NUM_AS, ITF_NUM_TOTAL };

#define EPNUM_AUDIO_IN        0x01
#define EP_ADDR_AUDIO_IN      (0x80 | EPNUM_AUDIO_IN)

#define AUDIO_N_CHANNELS      6
#define AUDIO_SAMPLE_RATE     48000
#define AUDIO_BYTES_PER_SAMPLE 3                          // 24-bit packed
#define AUDIO_PACKET_SIZE     ((AUDIO_SAMPLE_RATE/1000) * AUDIO_N_CHANNELS * AUDIO_BYTES_PER_SAMPLE) // 864

// Entity IDs
#define ID_CLK   0x01
#define ID_IT    0x02
#define ID_FU    0x03
#define ID_OT    0x04

// AC class-specific lengths
#define LEN_CS_AC_HEADER      9
#define LEN_CS_CLK_SRC        8
#define LEN_CS_IT             17
// Feature Unit: bLength = 6 + (N+1)*4   (iFeature je zahrnut)
#define LEN_CS_FU             (6 + ((AUDIO_N_CHANNELS + 1) * 4))      // 34
#define LEN_CS_OT             12
#define AC_CS_TOTAL           (LEN_CS_AC_HEADER + LEN_CS_CLK_SRC + LEN_CS_IT + LEN_CS_FU + LEN_CS_OT) // 80

static uint8_t const desc_configuration[] = {
  // ---- Standard Configuration ----
  9, TUSB_DESC_CONFIGURATION,
  0x00, 0x00,                    // wTotalLength -> doplní CB
  ITF_NUM_TOTAL,                 // bNumInterfaces = 2 (AC + AS)
  1, 0, 0x80, 50,                // cfg value, iCfg, attributes, 100 mA

  // ---- Standard AC Interface (alt 0, no EP) ----
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AC, 0, 0,
  TUSB_CLASS_AUDIO, 0x01, 0x20,  // AUDIO / AUDIOCONTROL / UAC2
  0,

  // ---- Class-Specific AC Header (UAC2) ----
  LEN_CS_AC_HEADER, 0x24, 0x01,  // CS_INTERFACE, HEADER
  0x00, 0x02,                    // bcdADC = 0x0200
  (uint8_t)(AC_CS_TOTAL & 0xFF), (uint8_t)(AC_CS_TOTAL >> 8), // wTotalLength = 80
  1, ITF_NUM_AS,                 // bInCollection=1, baInterfaceNr=AS

  // ---- Clock Source (internal, fixed 48k) ----
  LEN_CS_CLK_SRC, 0x24, 0x0A, ID_CLK,
  0x03,                          // bmAttributes: internal clock, not recoverable
  0x07,                          // bmControls: freq & valid read-only
  0x00,                          // bAssocTerminal
  0x00,                          // iClockSource

  // ---- Input Terminal (Microphone) ----
  LEN_CS_IT, 0x24, 0x02, ID_IT,
  0x01, 0x02,                    // wTerminalType = Microphone (0x0201)
  0x00,                          // bAssocTerminal
  ID_CLK,                        // bCSourceID
  AUDIO_N_CHANNELS,              // bNrChannels = 6
  0x00, 0x00, 0x00, 0x00,        // bmChannelConfig (discrete/undefined)
  0x00,                          // iChannelNames
  0x00, 0x00,                    // bmControls
  0x00,                          // iTerminal

  // ---- Feature Unit (Master + 6 channels) ----
  LEN_CS_FU, 0x24, 0x06, ID_FU, ID_IT,
  // bmaControls(0) Master: Mute+Volume
  0x03, 0x00, 0x00, 0x00,
  // bmaControls(1..6): 0
  0x00,0x00,0x00,0x00,  // ch1
  0x00,0x00,0x00,0x00,  // ch2
  0x00,0x00,0x00,0x00,  // ch3
  0x00,0x00,0x00,0x00,  // ch4
  0x00,0x00,0x00,0x00,  // ch5
  0x00,0x00,0x00,0x00,  // ch6
  // iFeature
  0x00,

  // ---- Output Terminal (to USB Streaming) ----
  LEN_CS_OT, 0x24, 0x03, ID_OT,
  0x01, 0x01,                    // wTerminalType = USB Streaming (0x0101)
  0x00,                          // bAssocTerminal
  ID_FU,                         // bSourceID
  ID_CLK,                        // bCSourceID
  0x00, 0x00,                    // bmControls
  0x00,                          // iTerminal

  // ---- AS Interface, alt 0 (zero bandwidth) ----
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AS, 0, 0,
  TUSB_CLASS_AUDIO, 0x02, 0x20, 0,

  // ---- AS Interface, alt 1 (operational) ----
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AS, 1, 1,              // 1 isoch IN EP
  TUSB_CLASS_AUDIO, 0x02, 0x20, 0,

  // ---- CS AS General ----
  16, 0x24, 0x01,                // CS_INTERFACE, AS_GENERAL
  ID_OT,                         // bTerminalLink
  0x00,                          // bmControls (none)
  0x01,                          // bFormatType = FORMAT_TYPE_I
  0x01, 0x00, 0x00, 0x00,        // bmFormats = PCM (bit 0)
  AUDIO_N_CHANNELS,              // bNrChannels = 6
  0x00, 0x00, 0x00, 0x00,        // bmChannelConfig = none
  0x00,                          // iChannelNames

  // ---- Type I Format (24-bit packed) ----
  6, 0x24, 0x02, 0x01,           // CS_INTERFACE, FORMAT_TYPE, FORMAT_TYPE_I
  AUDIO_BYTES_PER_SAMPLE,        // bSubslotSize = 3
  24,                            // bBitResolution = 24

  // ---- Standard ISO IN Endpoint (Async, Data) ----
  7, TUSB_DESC_ENDPOINT,
  EP_ADDR_AUDIO_IN,
  0x05,                          // Isochronous, Async, Data
  (AUDIO_PACKET_SIZE & 0xFF), (AUDIO_PACKET_SIZE >> 8), // 864
  0x01,                          // 1 ms

  // ---- CS ISO IN Endpoint ----
  8, 0x25, 0x01,                 // CS_ENDPOINT, EP_GENERAL
  0x00,                          // bmAttributes
  0x00,                          // bmControls
  0x00,                          // bLockDelayUnits
  0x00, 0x00,                    // wLockDelay
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

// ---------------- String descriptors ----------------
static char const* string_desc[] = {
  (const char[]){ 0x09, 0x04 },   // 0: English (US)
  "het68",                        // 1: Manufacturer
  "Pico 6ch Microphone 48k/24",   // 2: Product
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
