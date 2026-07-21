// buzzer.c — PS1240 piezo synchronisation beacon. See buzzer.h.
//
// Drive scheme (software H-bridge):
//   GP6 = PWM slice channel A, GP7 = same slice channel B.
//   Carrier = 50%% square at BUZZER_CARRIER_HZ (PS1240 resonance ~4 kHz).
//   - SILENT  : both channels same polarity  -> pins in phase -> 0 V across
//               the element -> no sound.
//   - DRIVE   : channels opposite polarity    -> differential ±VDD -> sound.
//   BPSK      : flipping which channel is inverted flips the carrier phase by
//               180°, i.e. encodes one PN chip.
//
// A single repeating hardware timer at the chip rate advances the PN code and
// the beacon schedule. The ISR only writes PWM polarity registers (a few
// cycles) — it never blocks, never touches UART, and never allocates, so it is
// safe alongside the realtime USB/I2S path on core0.
#include "buzzer.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include <stddef.h>

#define BUZZER_PIN_A        6u
#define BUZZER_PIN_B        7u
#define BUZZER_CARRIER_HZ   4000u     // PS1240 resonance
#define BUZZER_CYCLES_CHIP  8u        // carrier cycles per PN chip
#define BUZZER_PN_LEN       127u      // m-sequence length (LFSR x^7 + x^6 + 1)
#define BUZZER_PERIOD_MS    1000u     // one beacon burst per second

// Chip period in microseconds: BUZZER_CYCLES_CHIP carrier cycles.
#define BUZZER_CHIP_US      ((BUZZER_CYCLES_CHIP * 1000000u) / BUZZER_CARRIER_HZ)
// Idle chips between bursts (period minus burst length).
#define BUZZER_IDLE_CHIPS   (((BUZZER_PERIOD_MS * 1000u) / BUZZER_CHIP_US) > BUZZER_PN_LEN \
                             ? ((BUZZER_PERIOD_MS * 1000u) / BUZZER_CHIP_US) - BUZZER_PN_LEN \
                             : 1u)

static uint            s_slice;
static uint8_t         s_pn[BUZZER_PN_LEN];
static repeating_timer_t s_timer;

static volatile int32_t  s_chip = -1;          // -1 = idle, else 0..PN_LEN-1
static volatile uint32_t s_idle = 0;
static volatile uint32_t s_beacons = 0;

// --- Chip-stream TX state (acoustic link) -----------------------------------
static const uint8_t   *volatile s_stream;      // chip buffer (referenced)
static volatile uint32_t s_stream_len;
static volatile uint32_t s_stream_idx;
static volatile bool     s_stream_active;
static volatile bool     s_stream_pending;      // commit flag; ISR starts next tick
static volatile uint64_t s_stream_start_us;

// Differential drive for one PN chip: chip=1 -> phase 0, chip=0 -> phase 180.
static inline void buzzer_set_drive(uint8_t chip) {
    // (a_inv, b_inv): (false,true) and (true,false) are the two opposite
    // differential phases; both produce sound, 180° apart.
    pwm_set_output_polarity(s_slice, chip == 0u, chip != 0u);
}

// In-phase on both pins -> no differential voltage -> silent.
static inline void buzzer_set_silent(void) {
    pwm_set_output_polarity(s_slice, false, false);
}

static void buzzer_build_pn(void) {
    // 7-bit Fibonacci LFSR, taps 7 and 6 -> maximal length 127.
    uint8_t lfsr = 0x7Fu;
    for (uint32_t i = 0; i < BUZZER_PN_LEN; i++) {
        uint8_t bit = (uint8_t)(((lfsr >> 6) ^ (lfsr >> 5)) & 1u);
        s_pn[i] = (uint8_t)(lfsr & 1u);
        lfsr = (uint8_t)(((lfsr << 1) | bit) & 0x7Fu);
    }
}

static bool buzzer_tick(repeating_timer_t *t) {
    (void)t;

    // Acoustic-link chip stream pre-empts the periodic PN beacon.
    if (s_stream_pending && !s_stream_active) {
        s_stream_active = true;
        s_stream_pending = false;
        s_stream_idx = 0;
    }
    if (s_stream_active) {
        uint32_t idx = s_stream_idx;
        if (idx == 0u) s_stream_start_us = time_us_64();
        buzzer_set_drive(s_stream[idx]);
        idx++;
        s_stream_idx = idx;
        if (idx >= s_stream_len) {
            s_stream_active = false;
            buzzer_set_silent();
            // Do not immediately fire the beacon on top of the stream tail.
            s_chip = -1;
            s_idle = 0;
        }
        return true;
    }

    if (s_chip >= 0) {
        buzzer_set_drive(s_pn[s_chip]);
        s_chip++;
        if ((uint32_t)s_chip >= BUZZER_PN_LEN) {
            s_chip = -1;
            s_idle = 0;
            s_beacons++;
            buzzer_set_silent();
        }
    } else {
        if (++s_idle >= BUZZER_IDLE_CHIPS) {
            s_chip = 0;   // start a new burst on the next tick
        }
    }
    return true;  // keep repeating
}

void buzzer_init(void) {
    buzzer_build_pn();

    gpio_set_function(BUZZER_PIN_A, GPIO_FUNC_PWM);
    gpio_set_function(BUZZER_PIN_B, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(BUZZER_PIN_A);   // GP6/GP7 share one slice

    uint32_t wrap = clock_get_hz(clk_sys) / BUZZER_CARRIER_HZ;
    if (wrap == 0u) wrap = 1u;
    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, (uint16_t)(wrap - 1u));
    pwm_init(s_slice, &c, false);
    pwm_set_both_levels(s_slice, (uint16_t)(wrap / 2u), (uint16_t)(wrap / 2u));
    buzzer_set_silent();
    pwm_set_enabled(s_slice, true);

    // Negative period => fixed cadence independent of callback duration.
    add_repeating_timer_us(-(int64_t)BUZZER_CHIP_US, buzzer_tick, NULL, &s_timer);
}

uint32_t buzzer_beacon_count(void) {
    return s_beacons;
}

bool buzzer_tx_chips(const uint8_t *chips, uint32_t n_chips) {
    if (chips == NULL || n_chips == 0u) return false;
    if (s_stream_active || s_stream_pending) return false;
    s_stream = chips;
    s_stream_len = n_chips;
    s_stream_idx = 0;
    // Commit last: the ISR only starts once s_stream_pending is observed true.
    s_stream_pending = true;
    return true;
}

bool buzzer_tx_busy(void) {
    return s_stream_active || s_stream_pending;
}

uint64_t buzzer_tx_start_us(void) {
    return s_stream_start_us;
}
