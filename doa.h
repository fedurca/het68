// doa.h — multi-source DOA + diarization. See doa.c / entity_store.h.
#pragma once
#include <stdint.h>

void doa_ring_push(const int16_t s6[6]);
void doa_start(void);

extern volatile uint32_t g_doa_out;
extern volatile uint32_t g_doa_nactive;
extern volatile uint32_t g_doa_iter;
extern volatile uint32_t g_doa_ndrone;
extern volatile uint32_t g_doa_nwalker;
extern volatile uint32_t g_doa_nvehicle;
extern volatile uint32_t g_doa_nbird;
extern volatile uint32_t g_doa_entity_id;
extern volatile uint32_t g_doa_wind;          // 1 when wind currently detected
extern volatile float    g_doa_wind_az;
extern volatile float    g_doa_wind_el;
extern volatile float    g_doa_wind_db;       // intensity dB
