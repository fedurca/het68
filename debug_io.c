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

void dbg_init(void) {
    uart_init(uart_default, 115200);
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
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
