// debug_io.c — UART (GP16) + USB CDC debug output.
#include "debug_io.h"
#include "tusb_config.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#if CFG_TUD_CDC
#include "tusb.h"
#endif

void dbg_init(void) {
    uart_init(uart_default, 115200);
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
}

void dbg_putc(char c) {
    // Never block the main loop / tud_task() waiting for UART hardware.
    for (int spin = 0; spin < 256 && !uart_is_writable(uart_default); spin++) { }
    if (uart_is_writable(uart_default)) {
        uart_putc_raw(uart_default, c);
    }
#if CFG_TUD_CDC
    if (tud_cdc_connected()) {
        tud_cdc_write_char(c);
    }
#endif
}

void dbg_flush(void) {
#if CFG_TUD_CDC
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
