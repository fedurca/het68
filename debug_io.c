// debug_io.c — debug output. UART (pins set via PICO_DEFAULT_UART_*_PIN) is the
// PRIMARY channel and is fully independent of the Pico USB stack, so it survives
// a USB/UAC2 freeze.
//
// USB CDC mirroring is OPTIONAL (HET68_DEBUG_CDC) and is intentionally OFF by
// default: when USB wedges, CDC writes are useless and must never delay the
// UART path that we rely on to debug exactly that freeze.
#include "debug_io.h"
#include "tusb_config.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/sync.h"

// Mirror debug to USB CDC as a secondary channel. Off by default — see header.
#ifndef HET68_DEBUG_CDC
#define HET68_DEBUG_CDC 0
#endif

#if HET68_DEBUG_CDC && CFG_TUD_CDC
#include "tusb.h"
#define HET68_DEBUG_CDC_ACTIVE 1
#else
#define HET68_DEBUG_CDC_ACTIVE 0
#endif

// Cross-core line lock. Claimed once in dbg_init(); both cores serialise full
// debug lines through it so UART output never interleaves.
static spin_lock_t *dbg_spin;

void dbg_init(void) {
    uart_init(uart_default, 115200);
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
    if (!dbg_spin) {
        dbg_spin = spin_lock_init((uint)spin_lock_claim_unused(true));
    }
}

uint32_t dbg_line_lock(void) {
    return dbg_spin ? spin_lock_blocking(dbg_spin) : 0u;
}

void dbg_line_unlock(uint32_t saved) {
    if (dbg_spin) spin_unlock(dbg_spin, saved);
}

void dbg_putc(char c) {
    // UART first, and never blocked by USB state. Bounded spin so the main loop
    // / tud_task() can never stall here even if the UART FIFO is wedged. The
    // bound must exceed one byte time at the configured baud (~87 us @115200,
    // i.e. tens of thousands of cycles) or the 32-byte TX FIFO overflows and we
    // drop characters. The HW FIFO always drains at the baud rate even with no
    // listener, so this can never hang in practice.
    for (uint32_t spin = 0; spin < 200000u && !uart_is_writable(uart_default); spin++) { }
    if (uart_is_writable(uart_default)) {
        uart_putc_raw(uart_default, c);
    }
#if HET68_DEBUG_CDC_ACTIVE
    if (tud_cdc_connected()) {
        tud_cdc_write_char(c);
    }
#endif
}

void dbg_flush(void) {
#if HET68_DEBUG_CDC_ACTIVE
    if (tud_cdc_connected()) {
        tud_cdc_write_flush();
    }
#endif
}

void dbg_puts(const char *s) {
    while (*s) dbg_putc(*s++);
    dbg_flush();
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

void dbg_puthex32(uint32_t v) {
    dbg_puts("0x");
    dbg_puthex8((uint8_t)(v >> 24));
    dbg_puthex8((uint8_t)(v >> 16));
    dbg_puthex8((uint8_t)(v >> 8));
    dbg_puthex8((uint8_t)v);
}

bool dbg_rx_available(void) {
    return uart_is_readable(uart_default);
}

int dbg_getc(void) {
    if (!uart_is_readable(uart_default)) return -1;
    return (int)uart_getc(uart_default);
}
