#include "tusb.h"

// =================== Device descriptor ===================
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // 0/0/0 + IAD (uvedeme třídu až v konfiguraci)
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    // libovolné VID/PID pro vývoj
    .idVendor           = 0xCafe,
    .idProduct          = 0x4066,  // "6ch mic"
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

// =================== Configuration descriptor ===================
// UAC2, 1x Audio Control IF + 1x Audio Streaming IF (alt 0/1), 1x ISO IN EP
// Parametry odpovídají tvému tusb_config.h:
//  - 6 kanálů, 16 kHz, 16 bit, 1ms rámec = 192 B/packet

enum
{
  ITF_NUM_AUDIO_CONTROL = 0,
  ITF_NUM_AUDIO_STREAMING,
  ITF_NUM_TOTAL
};

// Endpoint adresa (IN ISO):
#define EPNUM_AUDIO_IN       0x01  // fyz. EP1, IN
#define EP_ADDR_AUDIO_IN     (0x80 | EPNUM_AUDIO_IN)

#define AUDIO_N_CHANNELS     6
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_SAMPLE_BYTES   2     // 16 bit
#define AUDIO_PACKET_SIZE    ((AUDIO_SAMPLE_RATE/1000) * AUDIO_N_CHANNELS * AUDIO_SAMPLE_BYTES) // 192

// UAC2 entity IDs (libovolné != 0):
#define ID_CLK        0x01
#define ID_IT_MIC     0x02
#define ID_OT_USB     0x03
#define ID_FU_MIC     0x04

// Konfigurační + Audio Class specifické deskriptory
// Pozn.: TinyUSB poskytuje makra pro UAC2 deskriptory v class/audio (pokud máš aktuální SDK).
// Pokud by tvá verze makra neměla, tohle pole ponech stejné — je to „raw“ bajtové pole, které funguje univerzálně.

static uint8_t const desc_configuration[] =
{
  // --- Standard Configuration ---
  9,                      // bLength
  TUSB_DESC_CONFIGURATION,// bDescriptorType
  0, 0,                   // wTotalLength (doplníme níž pomocí sizeof)
  ITF_NUM_TOTAL,          // bNumInterfaces
  1,                      // bConfigurationValue
  0,                      // iConfiguration
  0x80,                   // bmAttributes (bus powered)
  50,                     // bMaxPower (100 mA)

  // --- IAD (Audio Function) ---
  8, TUSB_DESC_INTERFACE_ASSOCIATION,
  ITF_NUM_AUDIO_CONTROL,  // first interface
  2,                      // interface count (AC + AS)
  TUSB_CLASS_AUDIO, 0x00, 0x00,
  0,                      // iFunction

  // --- Standard AC Interface (Alt 0, no EP) ---
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AUDIO_CONTROL,
  0,                      // alternate
  0,                      // EP count
  TUSB_CLASS_AUDIO, 0x01, // AUDIO + AUDIOCONTROL
  0x20,                   // UAC2 protocol
  0,                      // iInterface

  // --- Class-Specific AC Header (UAC2) ---
  // bLength(9), CS_INTERFACE, HEADER, bcdADC(0x0200), wTotalLength(??), bInCollection(1), baInterfaceNr(AS IF)
  9, 0x24, 0x01, 0x00, 0x02,  // Header, UAC2
  0, 0,                       // wTotalLength (doplní host; pro jistotu nastavíme na 0)
  1, ITF_NUM_AUDIO_STREAMING, // 1 streaming IF

  // --- Clock Source Entity (ID_CLK) ---
  // len(8), CS_INTERFACE, CLOCK_SOURCE(0x0A), ID, bmAttr, bmControls, bAssocTerm, iClockSource
  8, 0x24, 0x0A, ID_CLK, 0x03, 0x07, 0x00, 0x00,

  // --- Input Terminal (Microphone) -> ID_IT_MIC ---
  // len(17), CS_INTERFACE, INPUT_TERMINAL(0x02), ID, wTerminalType(0x0201 microphone),
  // assocTerminal(0), clkID, bNrChannels(6), bmChannelConfig(0..3)=0 (unspecified positions), iChannelNames(0), bmControls(2B)=Mute|Volume(typ), iTerminal(0)
  17, 0x24, 0x02, ID_IT_MIC, 0x01, 0x02, 0x00, ID_CLK,
  AUDIO_N_CHANNELS, 0x00, 0x00, 0x00, 0x00,   // 6ch, no specific positions
  0x00, 0x00,                                 // bmControls
  0x00,                                       // iTerminal

  // --- Feature Unit (per-mic mute/volume) -> ID_FU_MIC ---
  // len(8+N), CS_INTERFACE, FEATURE_UNIT(0x06), ID, sourceID(IT), bmaControls(0..Nch) 4B per channel (set minimal)
  // Zde minimal: master only (4B), žádné per-channel => celkem 12 B (8 + 4)
  12, 0x24, 0x06, ID_FU_MIC, ID_IT_MIC,
  0x0F, 0x00, 0x00, 0x00,  // bmaControls(0): Mute+Volume (bitfield, zde zjednodušeno)
  0x00, 0x00, 0x00, 0x00,  // bmaControls(1) - not present (padding pro UAC2 compliance)
  // iFeature
  // (nic, protože jsme dali fixní délku, iFeature = 0 chybí — vyhovuje)

  // --- Output Terminal (to USB Streaming) -> ID_OT_USB ---
  // len(12), CS_INTERFACE, OUTPUT_TERMINAL(0x03), ID, wTerminalType(0x0101 USB streaming), assocTerm(0), sourceID(FU), clkID, bmControls(2B)=0, iTerminal=0
  12, 0x24, 0x03, ID_OT_USB, 0x01, 0x01, 0x00, ID_FU_MIC, ID_CLK, 0x00, 0x00, 0x00,

  // --- Standard AS Interface, alt 0 (zero bandwidth) ---
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AUDIO_STREAMING, 0, 0,
  TUSB_CLASS_AUDIO, 0x02, 0x20, 0,

  // --- Standard AS Interface, alt 1 (operational) ---
  9, TUSB_DESC_INTERFACE,
  ITF_NUM_AUDIO_STREAMING, 1, 1, // 1 EP (iso IN)
  TUSB_CLASS_AUDIO, 0x02, 0x20, 0,

  // --- Class-Specific AS General ---
  // len(16), CS_INTERFACE, AS_GENERAL(0x01), termID(OT_USB), bmControls(0), formatType(1=TYPE I),
  // bmFormats(4B: PCM = 1), bNrChannels(6), bmChannelConfig(4B=0), iChannelNames(0)
  16, 0x24, 0x01, ID_OT_USB, 0x00, 0x01,
  0x01, 0x00, 0x00, 0x00,  // PCM
  AUDIO_N_CHANNELS,
  0x00, 0x00, 0x00, 0x00,  // channel config none
  0x00,

  // --- Type I Format (UAC2) ---
  // len(6), CS_INTERFACE, FORMAT_TYPE(0x02), FORMAT_TYPE_I(0x01), subslot(2B), bitresolution(16)
  6, 0x24, 0x02, 0x01, AUDIO_SAMPLE_BYTES, 16,

  // --- Standard ISO IN Endpoint ---
  7, TUSB_DESC_ENDPOINT,         // std EP desc
  EP_ADDR_AUDIO_IN,              // EP 1 IN
  0x0D,                          // Isochronous, Adaptive, Data
  (AUDIO_PACKET_SIZE & 0xFF),    // wMaxPacketSize
  (AUDIO_PACKET_SIZE >> 8),
  0x01,                          // bInterval = 1 (1 ms)

  // --- Class-Specific ISO IN Endpoint ---
  // len(8), CS_ENDPOINT, EP_GENERAL(0x01), bmAttrib(0), bmControls(0), bLockDelayUnits(0), wLockDelay(0)
  8, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// doplníme správnou wTotalLength do config headeru
static uint8_t cfg_desc_buf[sizeof(desc_configuration)];

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  memcpy(cfg_desc_buf, desc_configuration, sizeof(desc_configuration));
  uint16_t const total_len = sizeof(desc_configuration);
  cfg_desc_buf[2] = (uint8_t)(total_len & 0xFF);
  cfg_desc_buf[3] = (uint8_t)(total_len >> 8);
  return cfg_desc_buf;
}

// =================== String descriptors ===================
static char const *string_desc[] =
{
  (const char[]) { 0x09, 0x04 }, // 0: supported language = EN-US
  "het68",                       // 1: Manufacturer
  "Pico 6ch Microphone",         // 2: Product
  "123456",                      // 3: Serial
  "UAC2 Microphone"              // 4: Audio Function
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;
  if (index == 0)
  {
    memcpy(&_desc_str[1], string_desc[0], 2);
    chr_count = 1;
  }
  else
  {
    if (index >= sizeof(string_desc)/sizeof(string_desc[0])) return NULL;
    const char* str = string_desc[index];

    // Cap at 31 chars
    chr_count = (uint8_t) strlen(str);
    if (chr_count > 31) chr_count = 31;

    for (uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
    _desc_str[1+chr_count] = 0;
  }

  // first word is header: len, type
  _desc_str[0] = (TUSB_DESC_STRING << 8) | ((2*chr_count + 2) & 0xFF);
  return _desc_str;
}
