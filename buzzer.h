// buzzer.h — PS1240 piezo synchronisation beacon (software H-bridge).
//
// One passive PS1240 element is driven differentially from two GPIOs (GP6/GP7)
// configured as the A/B outputs of a single PWM slice. Channel B runs with
// inverted polarity, so the two pins form a software H-bridge: the element sees
// ±VDD (twice the swing of a single-ended drive, ~+6 dB SPL). The PS1240
// resonates near 4 kHz, which is the carrier we use.
//
// The beacon periodically emits a BPSK-modulated maximal-length (m-)sequence
// burst. This pseudo-random code is what neighbouring nodes will later cross-
// correlate (matched filter) to recover time-of-flight for ranging and clock
// sync. The RX/ranging side is a later phase; this module only emits.
#pragma once
#include <stdint.h>

// Configure the PWM carrier + the chip-rate timer. Silent until the first
// scheduled beacon. Call once from core0 after stdio/clocks are up.
void buzzer_init(void);

// Number of beacon bursts emitted so far (for heartbeat/diagnostics).
uint32_t buzzer_beacon_count(void);
