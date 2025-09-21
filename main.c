// main.c — RP2040 + TinyUSB: 6ch test mic callback + minimální USB deskriptory
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"

// ---------------------------------------------------------------------------
// Parametry audio streamu (musí korespondovat s tvým záměrem)
// ---------------------------------------------------------------------------
enum {
  AUDIO_SAMPLE_RATE = 16000,   // Hz
  AUDIO_CHANNELS_TX = 6,       // počet kanálů do hosta (mikrofon)
  BYTES_PER_SAMPLE  = 2        // 16-bit
};

enum {
  USB_PKT_LEN = (AUDIO_SAMPLE_RATE/1000) * AUDIO_CHANNELS_TX * BYTES_PER_SAMPLE
};

static uint8_t usb_buf[USB_PKT_LEN];

// ---------------------------------------------------------------------------
// Jednoduchý generátor testovacích dat (6 kanálů, sinus s fázemi)
// ---------------------------------------------------------------------------
static const int16_t s_sine_lut[64] = {
  0, 3211, 6392, 9512, 12539, 15446, 18204, 20787,
  23170, 25329, 27244, 28898, 30273, 31356, 32137, 32609,
  32767, 32609, 32137, 31356, 30273, 28898, 27244, 25329,
  23170, 20787, 18204, 15446, 12539, 9512, 6392, 3211,
  0, -3211, -6392, -9512, -12539, -15446, -18204, -20787,
  -23170, -25329, -27244, -28898, -30273, -31356, -32137, -32609,
  -32767, -32609, -32137, -31356, -30273, -28898, -27244, -25329,
  -23170, -20787, -18204, -15446, -12539, -9512, -6392, -3211
};
static uint8_t s_phase[AUDIO_CHANNELS_TX] = {0};

static void fill_usb_buf_from_source(void)
{
  const uint32_t n_per_ms = AUDIO_SAMPLE_RATE/1000; // 16 vzorků/kanál/ms
  int16_t *out = (int16_t *)usb_buf;

  for (uint32_t n = 0; n < n_per_ms; ++n) {
    for (uint32_t ch = 0; ch < AUDIO_CHANNELS_TX; ++ch) {
      uint8_t idx = (uint8_t)((s_phase[ch] + ch*4) & 63);
      *out++ = s_sine_lut[idx];
      s_phase[ch] = (uint8_t)((s_phase[ch] + 1) & 63);
    }
  }

  // Sem později napojíš čtení z I2S/DMA ring-bufferu (interleaved ch0..ch5).
}

// ---------------------------------------------------------------------------
// TinyUSB AUDIO callback – správný podpis (viz audio_device.h)
// ---------------------------------------------------------------------------
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id,
                                   uint8_t ep_in, uint8_t cur_alt_setting)
{
  (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;

  fill_usb_buf_from_source();
  (void)tud_audio_write(usb_buf, sizeof(usb_buf)); // pošleme 1ms rámec
  return true;
}

// Volitelné „device“ callbacky (stav)
void tud_mount_cb(void)         { }
void tud_umount_cb(void)        { }
void tud_suspend_cb(bool en)    { (void)en; }
void tud_resume_cb(void)        { }

// ---------------------------------------------------------------------------
// *** POVINNÉ DESKRIPTOR CALLBACKY ***
// Tohle řeší linkovací chyby: tud_descriptor_device_cb / configuration_cb / string_cb
// Následuje minimální composite „Misc/IAD“ zařízení s 1 vendor IF bez EP.
// Enumerace projde; UAC2 rozhraní doplníme v další iteraci.
// ---------------------------------------------------------------------------

// Device descriptor
tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  // Composite přes IAD (běžná volba pro vícetřídní zařízení)
  .bcdUSB             = 0x0200,
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

  .idVendor           = 0xCafe,
  .idProduct          = 0x4000,
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

// Jednoduchý config descriptor: Config + 1x Interface (vendor, bez EP).
// (UAC2 rozhraní a ISO EP přidáme později.)
enum {
  ITF_NUM_VENDOR = 0,
  ITF_COUNT      = 1
};

#define CONFIG_TOTAL_LEN   (TUD_CONFIG_DESC_LEN + TUD_INTERFACE_DESC_LEN)

uint8_t const desc_configuration[] = {
  // Config
  TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, 100),

  // Interface 0: vendor-specific, no EP (placeholder)
  // bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface
  TUD_INTERFACE_DESCRIPTOR(ITF_NUM_VENDOR, 0, 0, 0xFF, 0x00, 0x00, 0)
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
  return desc_configuration;
}

// String descriptor
static char const *string_desc[] = {
  (const char[]){ 0x09, 0x04 },     // 0: Jazyk (0x0409 = EN-US)
  "het68",                           // 1: Manufacturer
  "RP2040 6ch Mic (stub)",          // 2: Product
  "123456",                         // 3: Serial
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void)langid;

  uint8_t chr_count = 0;

  if (index == 0) {
    // návrat jazyka
    _desc_str[1] = 0x0409;
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2*2);
    return _desc_str;
  }

  if (index < sizeof(string_desc)/sizeof(string_desc[0])) {
    const char* str = string_desc[index];

    // převod do UTF-16LE
    while (str[chr_count] && chr_count < 31) {
      _desc_str[1 + chr_count] = (uint8_t)str[chr_count];
      chr_count++;
    }
    _desc_str[0] = (TUSB_DESC_STRING << 8) | ((chr_count+1)*2);
    return _desc_str;
  }

  return NULL;
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main(void)
{
  stdio_init_all();
  tud_init(0);

  while (true) {
    tud_task();
  }
  return 0;
}
