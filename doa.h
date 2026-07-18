// doa.h — multi-source DOA + drone/human classifier on core1. See doa.c.
#pragma once
#include <stdint.h>

void doa_ring_push(const int16_t s6[6]);
void doa_start(void);

// Diagnostics (heartbeat on core0).
extern volatile uint32_t g_doa_out;       // UART report cycles
extern volatile uint32_t g_doa_nactive;   // mics active in last drone-band solve
extern volatile uint32_t g_doa_iter;      // core1 loop iterations (alive check)
extern volatile uint32_t g_doa_ndrone;    // live drone tracks
extern volatile uint32_t g_doa_nhuman;    // live human/footstep tracks
