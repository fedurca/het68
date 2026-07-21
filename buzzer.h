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
#include <stdbool.h>

// Configure the PWM carrier + the chip-rate timer. Silent until the first
// scheduled beacon. Call once from core0 after stdio/clocks are up.
void buzzer_init(void);

// Number of beacon bursts emitted so far (for heartbeat/diagnostics).
uint32_t buzzer_beacon_count(void);

// --- Chip-stream TX (acoustic link PHY) --------------------------------------
// Hand the chip-rate ISR an arbitrary BPSK chip sequence (each byte 0/1 selects
// the carrier phase). The chips play out one per chip tick, pre-empting the
// periodic PN beacon; the free-running beacon resumes when the stream ends.
//
// The buffer is referenced (not copied) and MUST stay valid until
// buzzer_tx_busy() returns false. Returns false if a stream is already active.
bool buzzer_tx_chips(const uint8_t *chips, uint32_t n_chips);

// True while a queued chip stream is pending or transmitting.
bool buzzer_tx_busy(void);

// Absolute time (microseconds since boot) at which chip 0 of the current or
// most recent stream was emitted. 0 until the first stream starts.
uint64_t buzzer_tx_start_us(void);
