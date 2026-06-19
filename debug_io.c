// debug_io.c — portable UART debug helpers for checkpoint instrumentation.
// Uses pico-sdk uart_putc_raw so the correct UART hardware address is used
// regardless of whether the target is RP2040 (UART0=0x40034000) or RP2350
// (UART0=0x40070000).
#include "debug_io.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

void dbg_putc(char c) {
    while (!uart_is_writable(uart_default)) { }
    uart_putc_raw(uart_default, c);
}

void dbg_puts(const char *s) {
    while (*s) dbg_putc(*s++);
}

void dbg_putu32(uint32_t v) {
    if (v == 0) { dbg_putc('0'); return; }
    char buf[10]; int n = 0;
    for (; v; v /= 10) buf[n++] = '0' + v % 10;
    while (n--) dbg_putc(buf[n]);
}

void dbg_puthex8(uint8_t v) {
    const char hex[] = "0123456789ABCDEF";
    dbg_putc(hex[v >> 4]);
    dbg_putc(hex[v & 0xF]);
}
