#pragma once
#include <stdint.h>
#include <stdbool.h>

// UART (GP0/GP1 via PICO_DEFAULT_UART_*_PIN) + optional USB CDC debug output.
void dbg_init(void);
void dbg_putc(char c);
void dbg_flush(void);
void dbg_puts(const char *s);
void dbg_putu32(uint32_t v);
void dbg_puthex8(uint8_t v);
void dbg_puthex32(uint32_t v);

// Non-blocking UART RX helpers (debug probe UART).
bool dbg_rx_available(void);
int dbg_getc(void);   // -1 if none

// Coarse, cross-core line lock so debug lines emitted from core0 (heartbeat)
// and core1 (DOA) never interleave. The lock does not mask IRQs and does not
// wait on the UART. Drain the byte queue from the core0 main loop:
//   dbg_poll();
// Wrap one full line:
//   uint32_t s = dbg_line_lock(); dbg_puts(...); ...; dbg_line_unlock(s);
uint32_t dbg_line_lock(void);
void dbg_line_unlock(uint32_t saved);

// Move queued bytes into the UART FIFO. Core 0 only; never blocks.
void dbg_poll(void);

// When async, dbg_putc only queues. Call once the USB main loop is running
// so a heartbeat cannot stall isochronous IN.
void dbg_tx_async(bool async);

// Optional telemetry gate (SRC / ENTITY / TRACKS). CLI replies always print.
void dbg_log_set(bool enabled);
bool dbg_log_enabled(void);

// Periodic status line. CLI replies always print.
void dbg_hb_set(bool enabled);
bool dbg_hb_enabled(void);

// Single-character checkpoint macro: writes 'X\n' to UART.
// Usable from any .c file that includes this header.
#define DBG_CP(letter) do { dbg_putc(letter); dbg_putc('\n'); } while(0)
