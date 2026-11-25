// plugin_flash.c — NS-side H68P staging + dual-slot flash program (audio-idle).
#include "het68_secure.h"
#include "het68_flash_map.h"
#include "h68p.h"
#include "h68p_pubkey.h"
#include "debug_io.h"

#include "psram.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/unique_id.h"
#include "pico.h"

#include <string.h>
#include <ctype.h>

// Stage in PSRAM past the 6 MiB DOA ring (8 MiB total ⇒ 2 MiB free).
#define HET68_PLUGIN_STAGE_OFF  (6u * 1024u * 1024u)
_Static_assert(HET68_PLUGIN_STAGE_OFF + HET68_PLUGIN_SLOT_BYTES <= HET68_PSRAM_BYTES,
               "H68P stage must fit in PSRAM after DOA ring");

static uint8_t *plugin_stage(void) {
    return (uint8_t *)(uintptr_t)(HET68_PSRAM_BASE + HET68_PLUGIN_STAGE_OFF);
}

static uint32_t g_stage_len;
static bool g_importing;

typedef enum { PF_IDLE = 0, PF_ERASE, PF_PROG, PF_DONE } pf_phase_t;
static pf_phase_t g_phase = PF_IDLE;
static uint32_t g_prog_off;
static uint32_t g_prog_slot; // 0 or 1
static uint32_t g_prog_len;
static bool g_erase_all;

typedef struct { uint32_t off; uint32_t len; } flash_op_t;
typedef struct {
    uint32_t flash_off;
    const uint8_t *src;
    uint32_t len;
} flash_prog_t;

static void __not_in_flash_func(flash_erase_cb)(void *arg) {
    flash_op_t *op = (flash_op_t *)arg;
    flash_range_erase(op->off, op->len);
}

static void __not_in_flash_func(flash_prog_page_cb)(void *arg) {
    flash_prog_t *op = (flash_prog_t *)arg;
    flash_range_program(op->flash_off, op->src, op->len);
}

static uint32_t slot_off(uint32_t slot) {
    return (slot == 0u) ? HET68_PLUGIN_SLOT0_OFF : HET68_PLUGIN_SLOT1_OFF;
}

static const uint8_t *slot_xip(uint32_t slot) {
    return (const uint8_t *)(XIP_BASE + slot_off(slot));
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool het68_plugin_import_begin(void) {
    if (g_phase != PF_IDLE) return false;
    g_stage_len = 0;
    g_importing = true;
    memset(plugin_stage(), 0xff, HET68_PLUGIN_SLOT_BYTES);
    return true;
}

void het68_plugin_import_abort(void) {
    g_importing = false;
    g_stage_len = 0;
}

bool het68_plugin_import_hex_line(const char *line) {
    if (!g_importing || !line) return false;
    // Accept "H68PHEX <hex>" or raw hex.
    if (strncmp(line, "H68PHEX ", 8) == 0) line += 8;
    while (*line == ' ') line++;
    while (*line && *line != ' ') {
        int hi = hex_nibble(*line++);
        if (hi < 0 || !*line) return false;
        int lo = hex_nibble(*line++);
        if (lo < 0) return false;
        if (g_stage_len >= HET68_PLUGIN_SLOT_BYTES) return false;
        plugin_stage()[g_stage_len++] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool schedule_program(uint32_t slot, uint32_t len) {
    if (g_phase != PF_IDLE) return false;
    if (len == 0u || len > HET68_PLUGIN_SLOT_BYTES) return false;
    g_prog_slot = slot;
    g_prog_len = (len + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
    if (g_prog_len > HET68_PLUGIN_SLOT_BYTES) return false;
    // Pad stage with 0xFF out to page multiple.
    for (uint32_t i = len; i < g_prog_len; i++) plugin_stage()[i] = 0xffu;
    g_prog_off = 0;
    g_phase = PF_ERASE;
    g_erase_all = false;
    return true;
}

bool het68_plugin_import_end(bool *programmed_out) {
    if (programmed_out) *programmed_out = false;
    if (!g_importing) return false;
    g_importing = false;

    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);

    h68p_view_t view;
    h68p_status_t st = h68p_verify(plugin_stage(), g_stage_len, uid.id, HET68_H68P_PUBKEY, &view);
    if (st != H68P_OK) {
        dbg_puts("DET PLUGIN IMPORT ERR: ");
        dbg_puts(h68p_status_str(st));
        dbg_putc('\n');
        return false;
    }

    // Pick the inactive slot (higher rollback wins on read; here alternate).
    uint32_t slot = 0;
    const uint8_t *s0 = slot_xip(0);
    const h68p_header_t *h0 = (const h68p_header_t *)s0;
    if (h0->magic == H68P_MAGIC) slot = 1;

    if (!schedule_program(slot, view.total_bytes)) {
        dbg_puts("DET PLUGIN IMPORT ERR: busy\n");
        return false;
    }
    if (programmed_out) *programmed_out = true;
    dbg_puts("DET PLUGIN IMPORT: programming slot=");
    dbg_putu32(slot);
    dbg_puts(" bytes=");
    dbg_putu32(view.total_bytes);
    dbg_puts(" (idle USB)\n");
    return true;
}

bool het68_plugin_erase_slots(void) {
    if (g_phase != PF_IDLE) return false;
    g_erase_all = true;
    g_prog_slot = 0;
    g_phase = PF_ERASE;
    return true;
}

void het68_plugin_flash_poll(bool usb_idle) {
    if (!usb_idle || g_phase == PF_IDLE || g_phase == PF_DONE) {
        if (g_phase == PF_DONE) g_phase = PF_IDLE;
        return;
    }

    if (g_phase == PF_ERASE) {
        if (g_erase_all) {
            flash_op_t op = { .off = slot_off(g_prog_slot), .len = HET68_PLUGIN_SLOT_BYTES };
            if (flash_safe_execute(flash_erase_cb, &op, 200) != PICO_OK) return;
            if (g_prog_slot == 0u) {
                g_prog_slot = 1u;
                return; // erase slot1 next poll
            }
            g_erase_all = false;
            g_phase = PF_DONE;
            dbg_puts("DET PLUGIN: slots erased\n");
            return;
        }
        flash_op_t op = { .off = slot_off(g_prog_slot), .len = HET68_PLUGIN_SLOT_BYTES };
        if (flash_safe_execute(flash_erase_cb, &op, 200) != PICO_OK) return;
        g_phase = PF_PROG;
        g_prog_off = 0;
        return;
    }

    if (g_phase == PF_PROG) {
        if (g_prog_off >= g_prog_len) {
            g_phase = PF_DONE;
            dbg_puts("DET PLUGIN: flash OK slot=");
            dbg_putu32(g_prog_slot);
            dbg_putc('\n');
            return;
        }
        flash_prog_t op = {
            .flash_off = slot_off(g_prog_slot) + g_prog_off,
            .src = plugin_stage() + g_prog_off,
            .len = FLASH_PAGE_SIZE
        };
        if (flash_safe_execute(flash_prog_page_cb, &op, 200) != PICO_OK) return;
        g_prog_off += FLASH_PAGE_SIZE;
    }
}

// Shared status filled by loader; also used here for UART dump.
static het68_plugin_status_t g_status;

void het68_plugin_status_set(const het68_plugin_status_t *s) {
    if (s) g_status = *s;
}

void het68_plugin_status_get(het68_plugin_status_t *out) {
    if (out) *out = g_status;
}

bool het68_plugin_pick_best(const uint8_t serial[H68P_SERIAL_LEN],
                            h68p_view_t *out, uint32_t *slot_out) {
    h68p_view_t best;
    uint32_t best_slot = 0;
    uint32_t best_rb = 0;
    bool have = false;
    for (uint32_t s = 0; s < 2u; s++) {
        h68p_view_t v;
        h68p_status_t st = h68p_verify(slot_xip(s), HET68_PLUGIN_SLOT_BYTES,
                                       serial, HET68_H68P_PUBKEY, &v);
        if (st != H68P_OK) continue;
        if (!have || v.hdr->rollback >= best_rb) {
            best = v;
            best_slot = s;
            best_rb = v.hdr->rollback;
            have = true;
        }
    }
    if (!have) return false;
    if (out) *out = best;
    if (slot_out) *slot_out = best_slot;
    return true;
}

void het68_plugin_status_uart(void) {
    het68_plugin_status_t st;
    het68_plugin_status_get(&st);
    uint32_t lock = dbg_line_lock();
    dbg_puts("DET PLUGIN src=");
    switch (st.source) {
    case HET68_PLUGIN_BUILTIN: dbg_puts("builtin"); break;
    case HET68_PLUGIN_FLASH: dbg_puts("flash"); break;
    case HET68_PLUGIN_INVALID: dbg_puts("invalid"); break;
    default: dbg_puts("none"); break;
    }
    dbg_puts(" status=");
    dbg_puts(h68p_status_str(st.last_status));
    dbg_puts(" slot=");
    dbg_putu32(st.slot);
    dbg_puts(" code=");
    dbg_putu32(st.code_size);
    dbg_puts(" rb=");
    dbg_putu32(st.rollback);
    dbg_puts(" core1=");
    dbg_puts(st.core1_running ? "1" : "0");
    dbg_puts(" serial=");
    for (uint32_t i = 0; i < H68P_SERIAL_LEN; i++) {
        static const char *hexd = "0123456789abcdef";
        dbg_putc(hexd[st.serial[i] >> 4]);
        dbg_putc(hexd[st.serial[i] & 0xf]);
    }
    dbg_putc('\n');
    dbg_line_unlock(lock);
}
