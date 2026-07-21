// buzzer.h — PS1240 piezo PHY (software H-bridge on GP6/GP7).
//
// The passive PS1240 resonates near 4 kHz. The acoustic link drives it with
// short BPSK chips whose carrier may hop among nearby frequencies (FHSS) so a
// full frame stays under ~100 ms and does not sound like a long steady tone.
//
// A repeating chip-rate timer advances either:
//   - an acoustic-link chip stream (phase + optional per-chip frequency), or
//   - a rare idle PN keepalive (disabled for long periods; the link owns beacons).
#pragma once
#include <stdint.h>
#include <stdbool.h>

void buzzer_init(void);
uint32_t buzzer_beacon_count(void);

// Queue a BPSK chip stream. Each byte is a phase bit (0/1). If freq_hz is
// non-NULL it must have n_chips entries (carrier Hz per chip); otherwise every
// chip uses the default ~4 kHz resonance. The buffers are referenced (not
// copied) and must stay valid until buzzer_tx_busy() is false.
bool buzzer_tx_chips(const uint8_t *phase_bits, uint32_t n_chips);
bool buzzer_tx_chips_fh(const uint8_t *phase_bits, const uint16_t *freq_hz,
                        uint32_t n_chips);

bool buzzer_tx_busy(void);
uint64_t buzzer_tx_start_us(void);

// Fixed chip dwell used by the PHY (microseconds). Air-time = n_chips * this.
uint32_t buzzer_chip_us(void);
