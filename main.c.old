// main.c
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

// ------------------------------------------------------------
// Parametry musí odpovídat tomu, co je v tusb_config.h + usb_descriptors.c
// 6 kanálů, 16 kHz, 16 bitů (2 B na vzorek), 1 USB rámec = 1 ms
// => 16 vzorků/ch/ms * 6 ch * 2 B = 192 B na paket
// ------------------------------------------------------------
#define AUDIO_N_CHANNELS      6
#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_FRAME_BYTES     ((AUDIO_SAMPLE_RATE/1000) * AUDIO_N_CHANNELS * AUDIO_BYTES_PER_SAMPLE)

static uint8_t usb_frame_buf[AUDIO_FRAME_BYTES];

// Jednoduché doplnění audio rámce (zatím ticho)
static inline void fill_audio_frame(void)
{
  // ticho = samé nuly
  memset(usb_frame_buf, 0, sizeof(usb_frame_buf));

  // Pokud chceš rychle ověřit, že data “tečou”, můžeš sem dát třeba rampu:
  // static uint16_t s = 0;
  // uint16_t* p = (uint16_t*)usb_frame_buf;
  // for (int i = 0; i < AUDIO_FRAME_BYTES/2; ++i) {
  //   p[i] = s;
  //   s += 256;
  // }
}

// ------------------------------------------------------------
// TinyUSB inicializace a hlavní smyčka
// ------------------------------------------------------------
int main(void)
{
  stdio_init_all();
  tusb_init();

  // volitelně LED blikání pro život
  const uint LED_PIN = PICO_DEFAULT_LED_PIN;
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, true);

  absolute_time_t next_toggle = make_timeout_time_ms(250);

  while (true) {
    tud_task(); // zpracování USB

    if (absolute_time_diff_us(get_absolute_time(), next_toggle) <= 0) {
      gpio_xor_mask(1u << LED_PIN);
      next_toggle = make_timeout_time_ms(250);
    }
  }
  return 0;
}

// ------------------------------------------------------------
// USB události (volitelné, jen pro logiku/ladění)
// ------------------------------------------------------------
void tud_mount_cb(void)
{
  // Zařízení připojeno – nic speciálního neděláme
}

void tud_umount_cb(void)
{
  // Zařízení odpojeno
}

void tud_suspend_cb(bool remote_wakeup_en)
{
  (void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
}

// ------------------------------------------------------------
// AUDIO (UAC2) callbacky – signatury musí přesně sedět s TinyUSB
// audio_device.h deklaruje:
//
// bool tud_audio_tx_done_pre_load_cb(uint8_t rhport,
//                                    uint8_t func_id,
//                                    uint8_t ep_in,
//                                    uint8_t cur_alt_setting);
//
// V tomto callbacku připravíme a zapíšeme další isochronní IN paket.
// Vrátíme true = OK (TinyUSB může pokračovat).
// ------------------------------------------------------------
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport,
                                   uint8_t func_id,
                                   uint8_t ep_in,
                                   uint8_t cur_alt_setting)
{
  (void)rhport;
  (void)ep_in;

  // Streamujeme jen pokud je zvolený “operational” alt-setting (typicky 1)
  if (cur_alt_setting == 0) {
    return true; // zero-bandwidth: nic neposílat, ale OK
  }

  // (Nepovinné) Ověření, že je audio funkce opravdu připojená
  if (!tud_audio_n_mounted(func_id)) {
    return true;
  }

  // Naplnit 1 ms rámec a poslat ho hostovi
  fill_audio_frame();
  (void)tud_audio_write(usb_frame_buf, sizeof(usb_frame_buf));

  return true;
}

// ------------------------------------------------------------
// (Nepovinné) Pokud chceš reagovat na změnu alt-settingu rozhraní,
// TinyUSB má ještě další slabé (weak) callbacky. Není nutné je definovat.
// Základní stream výše funguje sám o sobě.
// ------------------------------------------------------------
