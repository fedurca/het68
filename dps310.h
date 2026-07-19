// dps310.h — Grove DPS310 barometer on I2C1 (GP2 SDA / GP3 SCL).
//
// Seeed 101020812 — Infineon DPS310, I2C default 0x77 (alt 0x76).
// Only claimed when GP2/GP3 are free and a device responds at probe.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DPS310_I2C_SDA_PIN 2u
#define DPS310_I2C_SCL_PIN 3u

typedef struct {
    float pressure_pa;   // compensated pressure (Pa)
    float temperature_c; // compensated temperature (°C)
    uint32_t age_ms;     // ms since last successful sample
} dps310_sample_t;

// Probe I2C, soft-reset, load coeffs, start continuous mode.
// Returns false if pins busy / no ACK / wrong product ID.
bool dps310_init(void);

bool dps310_available(void);
uint8_t dps310_i2c_addr(void); // 0 if not available

// Non-blocking: refresh cached sample when PRS_RDY (safe from main loop).
void dps310_poll(void);

bool dps310_read(dps310_sample_t *out); // false if never sampled
float dps310_pressure_hpa(void);        // 0 if none
float dps310_temperature_c(void);       // 0 if none
float dps310_altitude_m(void);          // rough QNH=1013.25 hPa; 0 if none

// Relative humidity fraction 0..1 used for moist-air c(T,p,RH).
// DPS310 has no RH sensor — default 0.50; override via dps310_set_rh() / CLI.
void dps310_set_rh(float rh_frac);
float dps310_rh(void);

// Moist-air speed of sound (m/s) from T+P (DPS310) and configured RH, or 0 if none.
// Arden-Buck p_sat → x_w = RH*p_sat/p → Cramer/NPL-style:
//   c = 331.36 * sqrt(T_K/273.15) * (1 + 0.314*x_w + 0.037*x_w^2)
float dps310_speed_of_sound_m_s(void);

void dps310_dump_uart(void);
