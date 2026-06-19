#pragma once
#include <stdint.h>

// Portable UART debug output — works on RP2040 and RP2350.
void dbg_putc(char c);
void dbg_puts(const char *s);
void dbg_putu32(uint32_t v);
void dbg_puthex8(uint8_t v);

// Single-character checkpoint macro: writes 'X\n' to UART.
// Usable from any .c file that includes this header.
#define DBG_CP(letter) do { dbg_putc(letter); dbg_putc('\n'); } while(0)
