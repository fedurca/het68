// het68_sau.c — TrustZone-M SAU + ACCESSCTRL plan for het68.
//
// Dual-ELF Secure/NS images are not linked yet. Programming SAU from this
// single firmware image without a Non-secure reset vector would brick the
// running process, so register writes stay behind HET68_SAU_PROGRAM=1.
#include "het68_secure.h"
#include "het68_flash_map.h"
#include "psram.h"

#include "pico.h"

#ifndef HET68_SAU_PROGRAM
#define HET68_SAU_PROGRAM 0
#endif

#if HET68_SAU_PROGRAM
// Armv8-M SAU register block (PPB). Avoid CMSIS core_cm33.h here — Pico SDK
// + -mcmse can disagree on inclusion order; these offsets are architectural.
#define HET68_SAU_CTRL   (*(volatile uint32_t *)0xE000EDD0u)
#define HET68_SAU_RNR    (*(volatile uint32_t *)0xE000EDD8u)
#define HET68_SAU_RBAR   (*(volatile uint32_t *)0xE000EDDCu)
#define HET68_SAU_RLAR   (*(volatile uint32_t *)0xE000EDE0u)
#define HET68_SAU_CTRL_ENABLE   (1u << 0)
#define HET68_SAU_RLAR_ENABLE   (1u << 0)
#define HET68_SAU_RLAR_NSC      (1u << 1)

// RP2350 ACCESSCTRL — leave debug open for lab (do not lock SWD).
#define HET68_ACCESSCTRL_LOCK   (*(volatile uint32_t *)0x40060000u)

static void sau_region(uint32_t idx, uint32_t base, uint32_t limit_inclusive,
                       bool nsc) {
    HET68_SAU_RNR = idx & 0xFFu;
    HET68_SAU_RBAR = base & 0xFFFFFFE0u;
    uint32_t rlar = (limit_inclusive & 0xFFFFFFE0u) | HET68_SAU_RLAR_ENABLE;
    if (nsc) rlar |= HET68_SAU_RLAR_NSC;
    HET68_SAU_RLAR = rlar;
}
#endif

void het68_sau_configure(void) {
#if HET68_SAU_PROGRAM
    // Region plan (memory not covered by an NS region remains Secure):
    //   R1: NS SRAM lower half 0x20000000..0x2003FFFF (TinyUSB / NS app)
    //   R3: NS PSRAM           HET68_PSRAM_BASE .. +HET68_PSRAM_BYTES-1
    //   Secure plugin window   0x20040000..0x2007FFFF stays Secure by omission
    sau_region(1, 0x20000000u, 0x2003FFFFu, false);
    sau_region(3,
               (uint32_t)HET68_PSRAM_BASE,
               (uint32_t)HET68_PSRAM_BASE + (uint32_t)HET68_PSRAM_BYTES - 1u,
               false);
    (void)HET68_PLUGIN_SRAM_BASE;
    (void)HET68_PLUGIN_SRAM_BYTES;
    (void)HET68_ACCESSCTRL_LOCK; // lab: do not lock debug

    HET68_SAU_CTRL = HET68_SAU_CTRL_ENABLE; // ALLNS=0
    __asm volatile ("dsb");
    __asm volatile ("isb");
#else
    // Contract only — see docs/secure-boot-lab.md.
    (void)HET68_PLUGIN_SRAM_BASE;
    (void)HET68_PSRAM_BASE;
#endif
}
