// core1_launch.c — see core1_launch.h.
#include "core1_launch.h"
#include "debug_io.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

// Minimum settle after multicore_reset_core1() before launch (ms).
// Lab scan (OpenOCD flash, production trampoline path): probe loop OK at 0 ms;
// DOA verify needs 1 ms (0 ms passes 80 ms then stalls — caught by two-window verify).
#ifndef HET68_CORE1_SETTLE_MS
#define HET68_CORE1_SETTLE_MS  1u
#endif

#define CORE1_SETTLE_MARGIN_MS  0u
#define CORE1_VERIFY_MS         80u
#define CORE1_VERIFY_MIN_DELTA  50000u
#define CORE1_SETTLE_OK_ITER    500000u   // probe iter after 100 ms observation

volatile uint32_t g_core1_alive;
volatile uint32_t g_core1_hb;

static void (*s_user_entry)(void);

static void core1_trampoline(void) {
    het68_core1_setup();
    g_core1_alive = 1u;
    __dmb();
    s_user_entry();
    for (;;) {
        tight_loop_contents();
    }
}

static void core1_probe_entry(void) {
    het68_core1_setup();
    for (;;) {
        g_core1_hb++;
    }
}

void het68_core1_setup(void) {
    // Enable CP10/CP11 (FPU) on Cortex-M33 (RP2350). RP2040 (M0+) has no FPU —
    // CPACR is unimplemented; do not poke SCS there.
#if defined(PICO_RP2350) && PICO_RP2350
    *(volatile uint32_t *)0xE000ED88u |= (0xFu << 20);
    __asm volatile ("dsb");
    __asm volatile ("isb");
#endif
}

static void launch_with_settle_ms(uint32_t settle_ms, void (*entry)(void)) {
    multicore_reset_core1();
    // Reset alone fixes post-OpenOCD core1 state; optional quiesce before launch.
    if (settle_ms > 0u) {
        sleep_ms(settle_ms);
    }
    s_user_entry = entry;
    g_core1_alive = 0u;
    g_core1_hb = 0u;
    __dmb();
    multicore_launch_core1(core1_trampoline);
}

static bool wait_boot_token(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!g_core1_alive) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

static bool default_verify(void) {
    uint32_t t0 = g_core1_hb;
    sleep_ms(CORE1_VERIFY_MS);
    return (g_core1_hb - t0) >= CORE1_VERIFY_MIN_DELTA;
}

static bool try_launch(void (*entry)(void), uint32_t settle_ms,
                       het68_core1_verify_fn verify, uint32_t *used_ms) {
    launch_with_settle_ms(settle_ms, entry);
    if (!wait_boot_token(25u)) {
        return false;
    }
    if (!(verify ? verify() : default_verify())) {
        return false;
    }
    if (used_ms) {
        *used_ms = settle_ms;
    }
    return true;
}

bool het68_launch_core1_verify(void (*entry)(void), het68_core1_verify_fn verify) {
    static const uint8_t delays[] = {
        (uint8_t)(HET68_CORE1_SETTLE_MS + CORE1_SETTLE_MARGIN_MS),
        1u, 2u, 5u, 10u,
    };

    for (uint32_t i = 0; i < sizeof(delays); i++) {
        uint32_t used = 0u;
        if (try_launch(entry, (uint32_t)delays[i], verify, &used)) {
            uint32_t s = dbg_line_lock();
            dbg_puts("core1: launch OK settle_ms=");
            dbg_putu32(used);
            dbg_putc('\n');
            dbg_line_unlock(s);
            return true;
        }
        multicore_reset_core1();
        sleep_ms(5);
    }

    uint32_t s = dbg_line_lock();
    dbg_puts("core1: launch failed after retries\n");
    dbg_line_unlock(s);
    return false;
}

bool het68_launch_core1(void (*entry)(void)) {
    return het68_launch_core1_verify(entry, NULL);
}

static void scan_one(uint32_t settle_ms, bool use_us, uint32_t settle_us) {
    multicore_reset_core1();
    if (use_us) {
        if (settle_us > 0u) {
            busy_wait_us(settle_us);
        }
    } else if (settle_ms > 0u) {
        sleep_ms(settle_ms);
    }
    g_core1_alive = 0u;
    g_core1_hb = 0u;
    s_user_entry = core1_probe_entry;
    __dmb();
    multicore_launch_core1(core1_trampoline);
    if (!wait_boot_token(25u)) {
        dbg_puts("C1SETTLE ");
        if (use_us) { dbg_puts("us="); dbg_putu32(settle_us); }
        else { dbg_puts("ms="); dbg_putu32(settle_ms); }
        dbg_puts(" iter=0 NOBOOT\n");
        multicore_reset_core1();
        sleep_ms(5);
        return;
    }
    sleep_ms(100);
    uint32_t iter = g_core1_hb;
    multicore_reset_core1();
    sleep_ms(5);
    dbg_puts("C1SETTLE ");
    if (use_us) {
        dbg_puts("us=");
        dbg_putu32(settle_us);
    } else {
        dbg_puts("ms=");
        dbg_putu32(settle_ms);
    }
    dbg_puts(" iter=");
    dbg_putu32(iter);
    dbg_puts(iter >= CORE1_SETTLE_OK_ITER ? " OK\n" : " FAIL\n");
}

void het68_core1_settle_scan(void) {
    static const uint16_t delays_us[] = { 0, 100, 250, 500, 1000, 2000, 5000 };
    static const uint16_t delays_ms[] = { 0, 1, 2, 3, 4, 5, 8, 10, 15, 20, 30, 50, 80 };

    dbg_puts("=== core1 settle scan (post OpenOCD flash) ===\n");
    for (uint32_t i = 0; i < sizeof(delays_us) / sizeof(delays_us[0]); i++) {
        scan_one(0, true, (uint32_t)delays_us[i]);
    }
    for (uint32_t i = 0; i < sizeof(delays_ms) / sizeof(delays_ms[0]); i++) {
        scan_one((uint32_t)delays_ms[i], false, 0);
    }
    dbg_puts("=== scan done ===\n");
}
