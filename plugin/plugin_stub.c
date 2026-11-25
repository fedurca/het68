// plugin_stub.c — minimal H68P payload for per-device signing / upload tests.
#include <stdint.h>

void plugin_entry(void);

void plugin_entry(void) {
    for (;;) {
        __asm volatile("wfi");
    }
}

const volatile uint32_t plugin_marker = 0x50383648u; /* 'H68P' */
