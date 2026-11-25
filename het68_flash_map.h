// het68_flash_map.h — flash layout for platform + H68P detection plugin slots.
//
// End of flash (unchanged ACID dual-slots, 4 KiB sectors):
//   DRONE[-8,-7], BT[-6,-5], DET[-4,-3], ENT[-2,-1]
// Plugin dual slots sit immediately above that reserved tail.
#pragma once

#include <stdint.h>

// Pull board flash size when building firmware (pico SDK).
#ifndef PICO_FLASH_SIZE_BYTES
#include "pico.h"
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must be defined by the board"
#endif

#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096u
#endif

// Sectors reserved at the end for ACID + BTstack TLV (see CMakeLists).
#define HET68_FLASH_ACID_SECTORS 8u
#define HET68_FLASH_ACID_BYTES   (HET68_FLASH_ACID_SECTORS * FLASH_SECTOR_SIZE)

// Dual A/B slots for signed detection plugins (H68P).
#define HET68_PLUGIN_SLOT_SECTORS 128u /* 512 KiB per slot */
#define HET68_PLUGIN_SLOT_BYTES   (HET68_PLUGIN_SLOT_SECTORS * FLASH_SECTOR_SIZE)

#define HET68_PLUGIN_SLOT1_OFF \
    (PICO_FLASH_SIZE_BYTES - HET68_FLASH_ACID_BYTES - HET68_PLUGIN_SLOT_BYTES)
#define HET68_PLUGIN_SLOT0_OFF \
    (HET68_PLUGIN_SLOT1_OFF - HET68_PLUGIN_SLOT_BYTES)

// Platform image must stay below the plugin region.
#define HET68_PLATFORM_FLASH_MAX (HET68_PLUGIN_SLOT0_OFF)

// Secure SRAM load window for a verified plugin (RP2350: 0x20000000..).
#define HET68_PLUGIN_SRAM_BASE   0x20040000u
#define HET68_PLUGIN_SRAM_BYTES  0x00040000u /* 256 KiB */
