// debug_io.c — debug output. UART (pins set via PICO_DEFAULT_UART_*_PIN) is the
// PRIMARY channel and is fully independent of the Pico USB stack, so it survives
// a USB/UAC2 freeze.
//
// Bytes are queued in RAM and drained by dbg_poll() on core 0. Printing must not
// spin on the UART baud rate and must not mask IRQs: the I2S state machines stall
// (and lose left/right phase) if the CPU stops emptying the PIO FIFO, and the
// USB task misses isochronous frames if the main loop blocks for tens of ms.
//
// USB CDC mirroring is OPTIONAL (HET68_DEBUG_CDC) and is intentionally OFF by
// default: when USB wedges, CDC writes are useless and must never delay the
// UART path that we rely on to debug exactly that freeze.
#include "debug_io.h"
#include "tusb_config.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pico/multicore.h"

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

#define DBG_RING_SZ 4096u

static uint8_t dbg_ring[DBG_RING_SZ];
static volatile uint16_t dbg_ring_w;
static volatile uint16_t dbg_ring_r;
static volatile uint32_t dbg_ring_drop;
static volatile uint8_t dbg_line_owner;
static volatile bool g_log_enabled = true;
static volatile bool g_hb_enabled = true;
// Boot prints drain inline so the banner is visible before the main loop.
// Streaming switches this off: dbg_poll() then drains between USB tasks.
static volatile bool g_tx_async;

void dbg_init(void) {
    uart_init(uart_default, 115200);
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
    // Debug stays on across reboot until the operator sends HB OFF / LOG OFF.
    g_hb_enabled = true;
    g_log_enabled = true;
    g_tx_async = false;
}

void dbg_puts_chip_serial(void) {
    char id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2u + 1u];
    pico_get_unique_board_id_string(id, (uint)sizeof id);
    dbg_puts(id[0] ? id : "?");
}

void dbg_print_banner(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("het68 ");
#ifdef HET68_VERSION_STR
    dbg_puts(HET68_VERSION_STR);
#else
    dbg_puts("?");
#endif
    dbg_puts("  build ");
#ifdef HET68_BUILD_STAMP
    dbg_puts(HET68_BUILD_STAMP);
#else
    dbg_puts(__DATE__);
    dbg_putc(' ');
    dbg_puts(__TIME__);
#endif
    dbg_puts("  debug=");
    dbg_puts((g_hb_enabled && g_log_enabled) ? "on" : "partial");
    dbg_puts("  HB=");
    dbg_puts(g_hb_enabled ? "on" : "off");
    dbg_puts("  LOG=");
    dbg_puts(g_log_enabled ? "on" : "off");
    dbg_puts("  serial=");
    dbg_puts_chip_serial();
    dbg_putc('\n');
    dbg_line_unlock(lock);
}

void dbg_log_set(bool enabled) { g_log_enabled = enabled; }
bool dbg_log_enabled(void) { return g_log_enabled; }

void dbg_hb_set(bool enabled) { g_hb_enabled = enabled; }
bool dbg_hb_enabled(void) { return g_hb_enabled; }

void dbg_tx_async(bool async) { g_tx_async = async; }

uint32_t dbg_line_lock(void) {
    uint8_t me = (uint8_t)(get_core_num() + 1u);
    for (;;) {
        uint32_t irq = save_and_disable_interrupts();
        if (dbg_line_owner == 0u) {
            dbg_line_owner = me;
            restore_interrupts(irq);
            return 0u;
        }
        restore_interrupts(irq);
        tight_loop_contents();
    }
}

void dbg_line_unlock(uint32_t saved) {
    (void)saved;
    uint32_t irq = save_and_disable_interrupts();
    dbg_line_owner = 0u;
    restore_interrupts(irq);
}

static void ring_push(char c) {
    uint32_t irq = save_and_disable_interrupts();
    uint16_t next = (uint16_t)((dbg_ring_w + 1u) & (DBG_RING_SZ - 1u));
    if (next == dbg_ring_r) {
        dbg_ring_drop++;
        restore_interrupts(irq);
        return;
    }
    dbg_ring[dbg_ring_w] = (uint8_t)c;
    dbg_ring_w = next;
    restore_interrupts(irq);
}

static int ring_pop(void) {
    uint32_t irq = save_and_disable_interrupts();
    if (dbg_ring_r == dbg_ring_w) {
        restore_interrupts(irq);
        return -1;
    }
    int c = dbg_ring[dbg_ring_r];
    dbg_ring_r = (uint16_t)((dbg_ring_r + 1u) & (DBG_RING_SZ - 1u));
    restore_interrupts(irq);
    return c;
}

void dbg_poll(void) {
    if (get_core_num() != 0u) return;
    bool cdc_wrote = false;
    while (uart_is_writable(uart_default)) {
        int c = ring_pop();
        if (c < 0) break;
        uart_putc_raw(uart_default, (char)c);
#if HET68_DEBUG_CDC_ACTIVE
        // Non-blocking mirror. A full CDC buffer must not stall the UART or USB audio.
        if (tud_cdc_connected() && tud_cdc_write_available()) {
            tud_cdc_write_char((char)c);
            cdc_wrote = true;
        }
#endif
    }
#if HET68_DEBUG_CDC_ACTIVE
    if (cdc_wrote) tud_cdc_write_flush();
#endif
}

static void dbg_putc_raw(char c) {
    ring_push(c);
    if (get_core_num() != 0u) return;
    if (!g_tx_async) {
        while (dbg_ring_r != dbg_ring_w) {
            dbg_poll();
            if (!uart_is_writable(uart_default)) tight_loop_contents();
        }
    } else {
        dbg_poll();
    }
}

void dbg_putc(char c) {
    // Minicom and other raw terminals do not return to column 0 on LF alone,
    // so each following line starts where the previous one ended.
    if (c == '\n') dbg_putc_raw('\r');
    dbg_putc_raw(c);
}

void dbg_flush(void) {
    if (get_core_num() == 0u) dbg_poll();
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
    if (uart_is_readable(uart_default)) return true;
#if HET68_DEBUG_CDC_ACTIVE
    if (tud_cdc_connected() && tud_cdc_available()) return true;
#endif
    return false;
}

int dbg_getc(void) {
    if (uart_is_readable(uart_default)) return (int)uart_getc(uart_default);
#if HET68_DEBUG_CDC_ACTIVE
    if (tud_cdc_connected() && tud_cdc_available()) {
        uint8_t b = 0;
        if (tud_cdc_read(&b, 1) == 1) return (int)b;
    }
#endif
    return -1;
}
