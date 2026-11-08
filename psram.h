// psram.h — 8 MB QSPI PSRAM (APS6404) on Pimoroni Pico Plus 2 W, QMI CS1.
//
// Pico SDK 2.2.0 has no hardware_psram. This is the board bring-up: GPIO 47 as
// CS1, quad mode, XIP window at 0x11000000. No malloc.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HET68_PSRAM_BASE      0x11000000u
#define HET68_PSRAM_NOCACHE   0x15000000u
#define HET68_PSRAM_BYTES     (8u * 1024u * 1024u)

bool het68_psram_init(void);
size_t het68_psram_size(void);

// Rewrite QMI window 1 after a flash_safe_execute (bootrom may clobber M1).
void het68_psram_restore(void);
