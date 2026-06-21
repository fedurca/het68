// doa.h — direction-of-arrival estimator on core1. See doa.c.
#pragma once
#include <stdint.h>

void doa_ring_push(const int16_t s6[6]);
void doa_start(void);

// Diagnostics (heartbeat on core0).
extern volatile uint32_t g_doa_out;
extern volatile uint32_t g_doa_nactive;
extern volatile uint32_t g_doa_iter;      // core1 loop iterations (alive check)
