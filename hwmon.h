// hwmon.h — UART hardware monitor for Pimoroni Pico Plus 2 W.
// CPU figures are DWT cycle utilisation (share of clk_sys not spent in the
// core1 idle spin). Current/power are estimates: the board has VSYS sense
// but no current shunt.
#pragma once
#include <stdint.h>
#include <stdbool.h>

void hwmon_init(void);
void hwmon_dwt_on(void);
void hwmon_account(void);
void hwmon_idle_spin(void);
void hwmon_poll(void);

void hwmon_print(void);
void hwmon_set_uart(bool on);
bool hwmon_uart_on(void);

// kHz. Retunes I2S (if running), UART baud, and the PSRAM QMI divider.
bool hwmon_set_clk_khz(uint32_t khz);
uint32_t hwmon_clk_khz(void);

uint32_t hwmon_psram_bytes(void);
uint32_t hwmon_psram_used(void);
void *hwmon_psram_alloc(uint32_t bytes);

// Implemented in main.c — keeps the I2S bit clock at 48 kHz after a sysclk change.
void i2s_retune_for_sysclk(void);
