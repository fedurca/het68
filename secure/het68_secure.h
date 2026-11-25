// het68_secure.h — TrustZone bring-up, H68P loader, NSC surface (NS callers).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "h68p.h"

typedef enum {
    HET68_PLUGIN_NONE = 0,
    HET68_PLUGIN_BUILTIN,
    HET68_PLUGIN_FLASH,
    HET68_PLUGIN_INVALID,
} het68_plugin_source_t;

typedef struct {
    het68_plugin_source_t source;
    h68p_status_t last_status;
    uint32_t slot;          // 0 or 1 when FLASH
    uint32_t code_size;
    uint32_t rollback;
    uint8_t serial[H68P_SERIAL_LEN];
    bool core1_running;
} het68_plugin_status_t;

// Configure SAU / ACCESSCTRL, optionally transition helpers. Safe to call once
// from Secure boot path before launching Core1 / NS work.
void het68_secure_init(void);

// Core0 Secure → Non-secure handoff (no-op unless HET68_SAU_PROGRAM=1).
void het68_enter_ns(uint32_t ns_msp, uint32_t ns_reset);

// Verify flash H68P (prefer newest valid slot) or fall back to builtin detection
// entry. Starts Core1 in the Secure world.
bool het68_secure_start(void);

// NSC veneers (cmse_nonsecure_entry) — NS may call these after SAU/NSC lands.
void het68_nsc_plugin_status(het68_plugin_status_t *out);
bool het68_nsc_plugin_erase_slots(void);
bool het68_nsc_plugin_import_begin(void);
bool het68_nsc_plugin_import_hex_line(const char *line);
bool het68_nsc_plugin_import_end(bool *programmed_out);

// NS-visible status snapshot (filled by Secure side / shared mailbox).
void het68_plugin_status_get(het68_plugin_status_t *out);

// Opportunistic flash programming of an uploaded H68P image (NS). Call from
// main loop when USB audio is idle — mirrors ACID store polling.
void het68_plugin_flash_poll(bool usb_idle);

// Begin / feed / finish a CDC hex upload into a staging buffer, then program.
bool het68_plugin_import_begin(void);
bool het68_plugin_import_hex_line(const char *line);
bool het68_plugin_import_end(bool *programmed_out);
void het68_plugin_import_abort(void);

bool het68_plugin_erase_slots(void);
void het68_plugin_status_uart(void);

// NS mailbox helpers (no-op pull when builtin detection is linked).
void het68_mailbox_publish(void);
void het68_mailbox_pull_to_ns(void);
