// core1_launch.c — see core1_launch.h.
#include "core1_launch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

// After PSM reset core1 drains its FIFO and signals readiness; allow time for
// the bootrom handshake before launching application code (Debug Probe path).
#define CORE1_RESET_SETTLE_MS  50u

void het68_core1_setup(void) {
    // SDK enables FPU during core0 runtime init; core1 launched via FIFO does
    // not inherit CPACR — float math would fault without this.
    *(volatile uint32_t *)0xE000ED88u |= (0xFu << 20);
    __asm volatile ("dsb");
    __asm volatile ("isb");
}

void het68_launch_core1(void (*entry)(void)) {
    multicore_reset_core1();
    sleep_ms(CORE1_RESET_SETTLE_MS);
    multicore_launch_core1(entry);
}
