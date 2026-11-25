// het68_ns_transition.c — Core0 Secure → Non-secure handoff helpers.
//
// Lab builds keep a single Secure image (USB/I2S/CLI + loader). The eventual
// dual-ELF flow: Secure CRT0 configures SAU, then jumps to the NS reset
// handler with NS MSP via BXNS. Until that image exists, het68_enter_ns()
// is a documented no-op so callers can stay in one world.
#include "het68_secure.h"

#include "pico.h"

#include <stdint.h>

void het68_enter_ns(uint32_t ns_msp, uint32_t ns_reset) {
#if HET68_SAU_PROGRAM
    // Program NS MSP_NS then branch to NS entry (Thumb).
    __asm volatile (
        "msr msp_ns, %0\n"
        "mov r0, %1\n"
        "bic r0, r0, #1\n"
        "orr r0, r0, #1\n"   /* Thumb bit for BXNS target */
        "bxns r0\n"
        :
        : "r"(ns_msp), "r"(ns_reset)
        : "r0", "memory"
    );
    __builtin_unreachable();
#else
    (void)ns_msp;
    (void)ns_reset;
    // Single-image lab: remain Secure; NS transition deferred.
#endif
}
