// buzzer.c — PS1240 piezo PHY. See buzzer.h.
//
// Drive scheme (software H-bridge):
//   GP6 = PWM slice channel A, GP7 = same slice channel B.
//   - SILENT  : both channels same polarity  -> 0 V across the element
//   - DRIVE   : opposite polarity             -> differential ±VDD
//   BPSK      : flipping which channel is inverted flips carrier phase 180°
//
// Chip dwell is fixed (BUZZER_CHIP_US) independent of the instantaneous carrier
// so FHSS can retune the PWM wrap every chip without stretching air-time.
#include "buzzer.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include <stddef.h>

#define BUZZER_PIN_A         6u
#define BUZZER_PIN_B         7u
#define BUZZER_CARRIER_HZ    4000u     // PS1240 resonance (default / preamble)
#define BUZZER_CHIP_US       250u      // 0.25 ms/chip -> 279 chips ~= 70 ms
#define BUZZER_PN_LEN        31u       // short idle keepalive (rare)
#define BUZZER_PERIOD_MS     30000u    // idle PN at most once per 30 s

#define BUZZER_IDLE_CHIPS    (((BUZZER_PERIOD_MS * 1000u) / BUZZER_CHIP_US) > BUZZER_PN_LEN \
                              ? ((BUZZER_PERIOD_MS * 1000u) / BUZZER_CHIP_US) - BUZZER_PN_LEN \
                              : 1u)

static uint              s_slice;
static uint8_t           s_pn[BUZZER_PN_LEN];
static repeating_timer_t s_timer;
static uint32_t          s_cur_hz;

static volatile int32_t  s_chip = -1;
static volatile uint32_t s_idle = 0;
static volatile uint32_t s_beacons = 0;

static const uint8_t   *volatile s_stream_phase;
static const uint16_t  *volatile s_stream_freq;   // NULL => default carrier
static volatile uint32_t s_stream_len;
static volatile uint32_t s_stream_idx;
static volatile bool     s_stream_active;
static volatile bool     s_stream_pending;
static volatile uint64_t s_stream_start_us;

static inline void buzzer_set_drive(uint8_t phase_bit) {
    pwm_set_output_polarity(s_slice, phase_bit == 0u, phase_bit != 0u);
}

static inline void buzzer_set_silent(void) {
    pwm_set_output_polarity(s_slice, false, false);
}

static void buzzer_set_freq_hz(uint32_t hz) {
    if (hz < 1000u) hz = 1000u;
    if (hz > 12000u) hz = 12000u;
    if (hz == s_cur_hz) return;
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t wrap = sys / hz;
    if (wrap < 2u) wrap = 2u;
    if (wrap > 65536u) wrap = 65536u;
    pwm_set_wrap(s_slice, (uint16_t)(wrap - 1u));
    pwm_set_both_levels(s_slice, (uint16_t)(wrap / 2u), (uint16_t)(wrap / 2u));
    s_cur_hz = hz;
}

static void buzzer_build_pn(void) {
    // Degree-5 m-sequence (x^5 + x^2 + 1), length 31.
    uint8_t lfsr = 0x1Fu;
    for (uint32_t i = 0; i < BUZZER_PN_LEN; i++) {
        uint8_t bit = (uint8_t)(((lfsr >> 4) ^ (lfsr >> 1)) & 1u);
        s_pn[i] = (uint8_t)(lfsr & 1u);
        lfsr = (uint8_t)(((lfsr << 1) | bit) & 0x1Fu);
    }
}

static bool buzzer_tick(repeating_timer_t *t) {
    (void)t;

    if (s_stream_pending && !s_stream_active) {
        s_stream_active = true;
        s_stream_pending = false;
        s_stream_idx = 0;
    }
    if (s_stream_active) {
        uint32_t idx = s_stream_idx;
        if (idx == 0u) s_stream_start_us = time_us_64();
        uint32_t hz = BUZZER_CARRIER_HZ;
        if (s_stream_freq != NULL) hz = s_stream_freq[idx];
        buzzer_set_freq_hz(hz);
        buzzer_set_drive(s_stream_phase[idx]);
        idx++;
        s_stream_idx = idx;
        if (idx >= s_stream_len) {
            s_stream_active = false;
            buzzer_set_silent();
            buzzer_set_freq_hz(BUZZER_CARRIER_HZ);
            s_chip = -1;
            s_idle = 0;
        }
        return true;
    }

    // Rare idle keepalive (mostly silent — acoustic_link owns real beacons).
    if (s_chip >= 0) {
        buzzer_set_freq_hz(BUZZER_CARRIER_HZ);
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
            s_chip = 0;
        }
    }
    return true;
}

void buzzer_init(void) {
    buzzer_build_pn();
    s_cur_hz = 0;

    gpio_set_function(BUZZER_PIN_A, GPIO_FUNC_PWM);
    gpio_set_function(BUZZER_PIN_B, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(BUZZER_PIN_A);

    pwm_config c = pwm_get_default_config();
    pwm_init(s_slice, &c, false);
    buzzer_set_freq_hz(BUZZER_CARRIER_HZ);
    buzzer_set_silent();
    pwm_set_enabled(s_slice, true);

    add_repeating_timer_us(-(int64_t)BUZZER_CHIP_US, buzzer_tick, NULL, &s_timer);
}

uint32_t buzzer_beacon_count(void) {
    return s_beacons;
}

bool buzzer_tx_chips(const uint8_t *phase_bits, uint32_t n_chips) {
    return buzzer_tx_chips_fh(phase_bits, NULL, n_chips);
}

bool buzzer_tx_chips_fh(const uint8_t *phase_bits, const uint16_t *freq_hz,
                        uint32_t n_chips) {
    if (phase_bits == NULL || n_chips == 0u) return false;
    if (s_stream_active || s_stream_pending) return false;
    s_stream_phase = phase_bits;
    s_stream_freq = freq_hz;
    s_stream_len = n_chips;
    s_stream_idx = 0;
    s_stream_pending = true;
    return true;
}

bool buzzer_tx_busy(void) {
    return s_stream_active || s_stream_pending;
}

uint64_t buzzer_tx_start_us(void) {
    return s_stream_start_us;
}

uint32_t buzzer_chip_us(void) {
    return BUZZER_CHIP_US;
}
