// doa.h — direction-of-arrival estimator running on core1.
//
// The USB feed publishes each decoded 6-channel frame into a ring with
// doa_ring_push(). doa_service(), called repeatedly from the main loop,
// consumes windows of samples and estimates the 3D direction of the loudest
// broadband source by time-domain cross-correlation (TDOA) across the six
// microphones, printing azimuth/elevation to the debug UART.
//
// doa_service() is bounded (<1 ms per call) so it can be interleaved with
// tud_task() without disturbing the 1 ms USB audio cadence. No malloc, no
// blocking.
#pragma once
#include <stdint.h>

// Push one frame of six top-16-bit signed samples (one per channel).
void doa_ring_push(const int16_t s6[6]);

// Initialise geometry/state. Call once before the main loop.
void doa_init(void);

// Advance the analysis by one bounded slice. Call often from the main loop.
void doa_service(void);
