// hwmon.c — cycle utilisation, runtime sysclk, VSYS + estimated energy, SRAM, PSRAM.
#include "hwmon.h"
#include "debug_io.h"
#include "remote_id.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/m33.h"
#include "hardware/regs/m33.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <malloc.h>

#define PSRAM_CACHED   0x11000000u
#define PSRAM_NOCACHE  0x15000000u
#define PSRAM_BYTES    (8u * 1024u * 1024u)
#define SRAM_BYTES     (520u * 1024u)
#define CLK_MIN_KHZ    48000u
#define CLK_MAX_KHZ    150000u

extern char __data_start__;
extern char __bss_end__;
extern char __end__;
extern char __HeapLimit;

static volatile uint32_t g_idle_cyc[2];
static uint32_t g_last_cyc[2];
static uint32_t g_last_idle[2];
static volatile uint8_t g_cpu_pct[2];
static bool g_uart_on = true;
static uint32_t g_psram_bytes;
static uint32_t g_psram_used;
static uint64_t g_energy_mJ;
static absolute_time_t g_next_print;
static absolute_time_t g_last_energy;

static void __no_inline_not_in_flash_func(qmi_tx)(uint32_t word) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) {}
    qmi_hw->direct_tx = word;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {}
}

static uint8_t __no_inline_not_in_flash_func(qmi_rx)(void) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {}
    return (uint8_t)qmi_hw->direct_rx;
}

static void __no_inline_not_in_flash_func(qmi_direct_on)(void) {
    qmi_hw->direct_csr =
        (6u << QMI_DIRECT_CSR_CLKDIV_LSB) |
        QMI_DIRECT_CSR_EN_BITS |
        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) (void)qmi_hw->direct_rx;
}

static void __no_inline_not_in_flash_func(qmi_direct_off)(void) {
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
}

static void hwmon_psram_timing(void) {
    uint32_t hz = clock_get_hz(clk_sys);
    uint32_t div = (hz + 80000000u - 1u) / 80000000u;
    if (div < 2u) div = 2u;
    if (div > 255u) div = 255u;
    qmi_hw->m[1].timing =
        (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
        (1u << QMI_M1_TIMING_RXDELAY_LSB) |
        (2u << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (div << QMI_M1_TIMING_CLKDIV_LSB);
}

static void __no_inline_not_in_flash_func(psram_bringup)(void) {
    uint32_t irq = save_and_disable_interrupts();
    qmi_direct_on();

    qmi_tx(QMI_DIRECT_TX_NOPUSH_BITS | 0x9fu);
    uint8_t id[8];
    for (int i = 0; i < 8; i++) {
        qmi_tx(0);
        id[i] = qmi_rx();
    }
    bool known = false;
    for (int i = 0; i < 8; i++) {
        if (id[i] == 0x0du || id[i] == 0x5du) known = true;
    }
    if (known) qmi_tx(QMI_DIRECT_TX_NOPUSH_BITS | 0x35u);
    qmi_direct_off();
    restore_interrupts(irq);
    if (!known) return;

    hwmon_psram_timing();
    qmi_hw->m[1].rfmt =
        (1u << QMI_M1_RFMT_PREFIX_LEN_LSB) |
        (2u << QMI_M1_RFMT_ADDR_WIDTH_LSB) |
        (2u << QMI_M1_RFMT_DUMMY_WIDTH_LSB) |
        (6u << QMI_M1_RFMT_DUMMY_LEN_LSB) |
        (2u << QMI_M1_RFMT_DATA_WIDTH_LSB);
    qmi_hw->m[1].rcmd = 0xebu;
    qmi_hw->m[1].wfmt =
        (1u << QMI_M1_WFMT_PREFIX_LEN_LSB) |
        (2u << QMI_M1_WFMT_ADDR_WIDTH_LSB) |
        (2u << QMI_M1_WFMT_DATA_WIDTH_LSB);
    qmi_hw->m[1].wcmd = 0x38u;

    volatile uint32_t *probe = (volatile uint32_t *)PSRAM_NOCACHE;
    uint32_t prev = *probe;
    *probe = 0xA5A55A5Au;
    bool ok = (*probe == 0xA5A55A5Au);
    *probe = prev;
    if (ok) g_psram_bytes = PSRAM_BYTES;
}

void hwmon_dwt_on(void) {
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    uint core = get_core_num();
    if (core > 1u) return;
    g_last_cyc[core] = 0;
    g_last_idle[core] = 0;
}

void hwmon_account(void) {
    uint core = get_core_num();
    if (core > 1u) return;
    uint32_t now = m33_hw->dwt_cyccnt;
    uint32_t dc = now - g_last_cyc[core];
    uint32_t di = g_idle_cyc[core] - g_last_idle[core];
    if (dc < 200000u) return;
    if (di > dc) di = dc;
    g_cpu_pct[core] = (uint8_t)(((dc - di) * 100u) / dc);
    g_last_cyc[core] = now;
    g_last_idle[core] = g_idle_cyc[core];
}

void hwmon_idle_spin(void) {
    uint32_t a = m33_hw->dwt_cyccnt;
    tight_loop_contents();
    uint core = get_core_num();
    if (core < 2u) g_idle_cyc[core] += m33_hw->dwt_cyccnt - a;
}

static uint32_t vsys_mv(void) {
#if CYW43_USES_VSYS_PIN
    cyw43_thread_enter();
#endif
    adc_select_input(PICO_VSYS_PIN - 40);
    uint32_t raw = adc_read();
#if CYW43_USES_VSYS_PIN
    cyw43_thread_exit();
#endif
    return (raw * 3300u * 3u) / 4096u;
}

static uint32_t iest_ma(void) {
    uint32_t clk = clock_get_hz(clk_sys) / 1000000u;
    uint32_t load = (uint32_t)g_cpu_pct[0] + (uint32_t)g_cpu_pct[1];
    uint32_t ma = 18u + (clk * load * 12u) / 10000u;
    if (remote_id_available() && remote_id_enabled()) ma += 20u;
    return ma;
}

static void energy_tick(void) {
    absolute_time_t now = get_absolute_time();
    int64_t us = absolute_time_diff_us(g_last_energy, now);
    if (us < 200000) return;
    g_last_energy = now;
    uint32_t mv = vsys_mv();
    uint32_t ma = iest_ma();
    uint32_t ms = (uint32_t)(us / 1000);
    g_energy_mJ += ((uint64_t)mv * ma * ms) / 1000000ull;
}

void hwmon_print(void) {
    hwmon_account();
    energy_tick();
    uint32_t clk = clock_get_hz(clk_sys) / 1000u;
    uint32_t mv = vsys_mv();
    uint32_t ma = iest_ma();
    uint32_t mw = (mv * ma) / 1000u;
    uint32_t e10 = (uint32_t)((g_energy_mJ * 10ull) / 3600ull);

    uint32_t static_b = (uint32_t)(&__bss_end__ - &__data_start__);
    struct mallinfo mi = mallinfo();
    uint32_t heap_used = (uint32_t)mi.uordblks;
    uint32_t heap_arena = (uint32_t)(&__HeapLimit - &__end__);
    uint32_t ram_used = (uint32_t)(&__bss_end__ - (char *)0x20000000u) + heap_used;
    if (ram_used > SRAM_BYTES) ram_used = SRAM_BYTES;

    uint32_t lock = dbg_line_lock();
    dbg_puts("HW cpu0=");
    dbg_putu32(g_cpu_pct[0]);
    dbg_puts("% cpu1=");
    dbg_putu32(g_cpu_pct[1]);
    dbg_puts("% clk=");
    dbg_putu32(clk);
    dbg_puts("kHz vsys=");
    dbg_putu32(mv);
    dbg_puts("mV iest=");
    dbg_putu32(ma);
    dbg_puts("mA pest=");
    dbg_putu32(mw);
    dbg_puts("mW e=");
    dbg_putu32(e10 / 10u);
    dbg_putc('.');
    dbg_putu32(e10 % 10u);
    dbg_puts("mWh ram=");
    dbg_putu32(ram_used / 1024u);
    dbg_puts("/");
    dbg_putu32(SRAM_BYTES / 1024u);
    dbg_puts("kB static=");
    dbg_putu32(static_b / 1024u);
    dbg_puts("kB heap=");
    dbg_putu32(heap_used / 1024u);
    dbg_puts("/");
    dbg_putu32(heap_arena / 1024u);
    dbg_puts("kB psram=");
    dbg_putu32(g_psram_used / 1024u);
    dbg_puts("/");
    dbg_putu32(g_psram_bytes / 1024u);
    dbg_puts("kB\n");
    dbg_line_unlock(lock);
}

void hwmon_init(void) {
    hwmon_dwt_on();
    adc_init();
    adc_gpio_init(PICO_VSYS_PIN);
    gpio_set_function(PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    psram_bringup();
    g_last_energy = get_absolute_time();
    g_next_print = make_timeout_time_ms(1000);
    dbg_puts("HW: monitor on (cpu=DWT cycles, iest from VSYS model, no shunt)\n");
    if (g_psram_bytes)
        dbg_puts("HW: PSRAM 8MB at 0x11000000\n");
    else
        dbg_puts("HW: PSRAM probe failed\n");
}

void hwmon_poll(void) {
    hwmon_account();
    energy_tick();
    if (!g_uart_on) return;
    if (absolute_time_diff_us(get_absolute_time(), g_next_print) > 0) return;
    g_next_print = make_timeout_time_ms(1000);
    hwmon_print();
}

void hwmon_set_uart(bool on) { g_uart_on = on; }
bool hwmon_uart_on(void) { return g_uart_on; }
uint32_t hwmon_clk_khz(void) { return clock_get_hz(clk_sys) / 1000u; }
uint32_t hwmon_psram_bytes(void) { return g_psram_bytes; }
uint32_t hwmon_psram_used(void) { return g_psram_used; }

void *hwmon_psram_alloc(uint32_t bytes) {
    if (!g_psram_bytes || !bytes) return NULL;
    uint32_t at = (g_psram_used + 7u) & ~7u;
    if (at > g_psram_bytes || bytes > g_psram_bytes - at) return NULL;
    g_psram_used = at + bytes;
    return (void *)(PSRAM_CACHED + at);
}

bool hwmon_set_clk_khz(uint32_t khz) {
    if (khz < CLK_MIN_KHZ || khz > CLK_MAX_KHZ) return false;
    if (!set_sys_clock_khz(khz, false)) return false;
    uart_set_baudrate(uart_default, 115200);
    i2s_retune_for_sysclk();
    if (g_psram_bytes) hwmon_psram_timing();
    g_last_cyc[0] = m33_hw->dwt_cyccnt;
    g_last_idle[0] = g_idle_cyc[0];
    return true;
}
