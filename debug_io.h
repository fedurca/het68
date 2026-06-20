#pragma once
#include <stdint.h>

// UART (GP16) + optional USB CDC debug output.
void dbg_init(void);
void dbg_putc(char c);
void dbg_flush(void);
void dbg_puts(const char *s);
void dbg_putu32(uint32_t v);
void dbg_puthex8(uint8_t v);

// Single-character checkpoint macro: writes 'X\n' to UART.
// Usable from any .c file that includes this header.
#define DBG_CP(letter) do { dbg_putc(letter); dbg_putc('\n'); } while(0)
