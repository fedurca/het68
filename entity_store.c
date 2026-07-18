// entity_store.c — RAM-first gallery with opportunistic ACID dual-slot flash.
//
// Flash layout (end of primary flash):
//   slot0 = PICO_FLASH_SIZE_BYTES - 2*FLASH_SECTOR_SIZE
//   slot1 = PICO_FLASH_SIZE_BYTES - 1*FLASH_SECTOR_SIZE
// Each slot holds one entity_blob_t. Load picks highest seq with valid CRC.
// Saves never erase the active slot: write the inactive one page-by-page when
// USB audio is idle (alt=0), then the new seq/CRC makes it the winner.
#include "entity_store.h"
#include "debug_io.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <string.h>
#include <math.h>
#include <ctype.h>

#define ENTITY_MAGIC   0x45383648u  /* 'H68E' LE */
#define ENTITY_VERSION 2u

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must be defined by the board"
#endif

#define ENTITY_SLOT0_OFF  (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE)
#define ENTITY_SLOT1_OFF  (PICO_FLASH_SIZE_BYTES - 1u * FLASH_SECTOR_SIZE)

#define ENTITY_MATCH_MAX_DIST 0.55f

static entity_slot_t g_slots[ENTITY_STORE_MAX];
static uint32_t g_next_id = 1;
static uint32_t g_count = 0;
static uint32_t g_seq = 0;          // last successfully loaded/saved seq
static uint32_t g_active_slot = 0;  // 0 or 1 — last known good flash slot
static volatile bool g_dirty = false;

// Opportunistic save state machine (core0 only).
typedef enum {
    SAVE_IDLE = 0,
    SAVE_ERASE,
    SAVE_PROG
} save_phase_t;

static save_phase_t g_phase = SAVE_IDLE;
static uint32_t g_save_slot = 0;
static uint32_t g_prog_off = 0;
static uint8_t g_stage[FLASH_SECTOR_SIZE];
static entity_blob_t g_import_blob;
static uint32_t g_import_pos = 0;
static bool g_importing = false;

static void put_f2(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 100.0f + 0.5f);
    if (fp >= 100u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    if (fp < 10u) dbg_putc('0');
    dbg_putu32(fp);
}

const char *entity_class_name(entity_class_t c) {
    switch (c) {
        case ENT_HUMAN:    return "human";
        case ENT_CAT:      return "cat";
        case ENT_DOG:      return "dog";
        case ENT_ICE:      return "ice";
        case ENT_EV:       return "ev";
        case ENT_BIRD:     return "bird";
        case ENT_SONGBIRD: return "songbird";
        case ENT_CORVID:   return "corvid";
        default:           return "none";
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return ~crc;
}

uint32_t entity_blob_crc(const entity_blob_t *b) {
    entity_blob_t tmp = *b;
    tmp.crc = 0;
    return crc32_update(0, (const uint8_t *)&tmp, sizeof(tmp));
}

static void recount(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ENTITY_STORE_MAX; i++)
        if (g_slots[i].used) n++;
    g_count = n;
}

static bool slot_valid(const entity_blob_t *b) {
    if (b->magic != ENTITY_MAGIC) return false;
    if (b->version != ENTITY_VERSION) return false;
    if (b->count > ENTITY_STORE_MAX) return false;
    return entity_blob_crc(b) == b->crc;
}

static void load_from_flash(void) {
    const entity_blob_t *s0 = (const entity_blob_t *)(XIP_BASE + ENTITY_SLOT0_OFF);
    const entity_blob_t *s1 = (const entity_blob_t *)(XIP_BASE + ENTITY_SLOT1_OFF);
    memset(g_slots, 0, sizeof(g_slots));
    g_next_id = 1;
    g_count = 0;
    g_seq = 0;
    g_active_slot = 0;

    bool v0 = slot_valid(s0);
    bool v1 = slot_valid(s1);
    const entity_blob_t *best = NULL;
    if (v0 && v1) {
        best = (s1->seq >= s0->seq) ? s1 : s0;
        g_active_slot = (best == s1) ? 1u : 0u;
    } else if (v0) {
        best = s0;
        g_active_slot = 0;
    } else if (v1) {
        best = s1;
        g_active_slot = 1;
    }
    if (!best) return;

    memcpy(g_slots, best->slots, sizeof(g_slots));
    g_next_id = best->next_id ? best->next_id : 1;
    g_seq = best->seq;
    recount();
}

static void build_stage_blob(void) {
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) g_stage[i] = 0xFFu;
    entity_blob_t *b = (entity_blob_t *)g_stage;
    memset(b, 0, sizeof(*b));
    b->magic = ENTITY_MAGIC;
    b->version = ENTITY_VERSION;
    b->seq = g_seq + 1u;
    b->next_id = g_next_id;
    recount();
    b->count = g_count;
    memcpy(b->slots, g_slots, sizeof(g_slots));
    b->crc = entity_blob_crc(b);
}

typedef struct { uint32_t off; uint32_t len; } flash_op_t;

static void __not_in_flash_func(flash_erase_cb)(void *param) {
    flash_op_t *op = (flash_op_t *)param;
    flash_range_erase(op->off, FLASH_SECTOR_SIZE);
}

typedef struct {
    uint32_t flash_off; // absolute
    uint32_t stage_off; // into g_stage
    uint32_t len;
} flash_prog_t;

static void __not_in_flash_func(flash_prog_page_cb)(void *param) {
    flash_prog_t *op = (flash_prog_t *)param;
    flash_range_program(op->flash_off, &g_stage[op->stage_off], op->len);
}

void entity_store_core_init(void) {
    flash_safe_execute_core_init();
}

void entity_store_init(void) {
    load_from_flash();
    g_dirty = false;
    g_phase = SAVE_IDLE;
    g_importing = false;
}

bool entity_store_dirty(void) { return g_dirty; }
bool entity_store_saving(void) { return g_phase != SAVE_IDLE; }

void entity_store_dump_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== entity store (RAM");
    if (g_dirty) dbg_puts("+dirty");
    dbg_puts(") n=");
    dbg_putu32(g_count);
    dbg_puts(" next_id=");
    dbg_putu32(g_next_id);
    dbg_puts(" seq=");
    dbg_putu32(g_seq);
    dbg_puts(" flash_slot=");
    dbg_putu32(g_active_slot);
    dbg_puts(" ===\n");
    if (g_count == 0) dbg_puts("(empty)\n");
    for (uint32_t i = 0; i < ENTITY_STORE_MAX; i++) {
        if (!g_slots[i].used) continue;
        const entity_slot_t *s = &g_slots[i];
        dbg_puts("ENT id=");
        dbg_putu32(s->id);
        dbg_puts(" class=");
        dbg_puts(entity_class_name((entity_class_t)s->cls));
        dbg_puts(" hits=");
        dbg_putu32(s->hits);
        dbg_puts(" cadence=");
        put_f2(s->sig.cadence_hz);
        dbg_puts("Hz low=");
        put_f2(s->sig.low_ratio);
        dbg_puts(" mid=");
        put_f2(s->sig.mid_ratio);
        dbg_puts(" high=");
        put_f2(s->sig.high_ratio);
        dbg_puts(" lvl=");
        put_f2(s->sig.peak_db);
        dbg_puts("dB az=");
        put_f2(s->sig.az_n * 360.0f);
        dbg_puts(" el=");
        put_f2(s->sig.el_n * 180.0f - 90.0f);
        dbg_putc('\n');
    }
    dbg_puts("=== end entity store ===\n");
    dbg_line_unlock(lock);
}

static float sig_dist(const entity_sig_t *a, const entity_sig_t *b) {
    float d0 = (a->cadence_hz - b->cadence_hz) / 4.0f;
    float d1 = (a->peak_db - b->peak_db) / 20.0f;
    float d2 = a->low_ratio - b->low_ratio;
    float d3 = a->high_ratio - b->high_ratio;
    float d4 = a->mid_ratio - b->mid_ratio;
    float d5 = (a->crest - b->crest) / 8.0f;
    float d6 = a->az_n - b->az_n;
    if (d6 > 0.5f) d6 -= 1.0f;
    if (d6 < -0.5f) d6 += 1.0f;
    d6 *= 0.5f;
    float d7 = (a->el_n - b->el_n) * 0.5f;
    return sqrtf(d0*d0 + d1*d1 + d2*d2 + d3*d3 + d4*d4 + d5*d5 + d6*d6 + d7*d7);
}

static void sig_ema(entity_sig_t *dst, const entity_sig_t *src, float a) {
    dst->cadence_hz = (1-a)*dst->cadence_hz + a*src->cadence_hz;
    dst->peak_db    = (1-a)*dst->peak_db    + a*src->peak_db;
    dst->low_ratio  = (1-a)*dst->low_ratio  + a*src->low_ratio;
    dst->high_ratio = (1-a)*dst->high_ratio + a*src->high_ratio;
    dst->mid_ratio  = (1-a)*dst->mid_ratio  + a*src->mid_ratio;
    dst->crest      = (1-a)*dst->crest      + a*src->crest;
    dst->az_n       = (1-a)*dst->az_n       + a*src->az_n;
    dst->el_n       = (1-a)*dst->el_n       + a*src->el_n;
}

uint32_t entity_store_match_or_create(entity_class_t cls, const entity_sig_t *sig,
                                      float *match_out) {
    int best_i = -1;
    float best_d = ENTITY_MATCH_MAX_DIST;
    for (uint32_t i = 0; i < ENTITY_STORE_MAX; i++) {
        if (!g_slots[i].used) continue;
        if ((entity_class_t)g_slots[i].cls != cls) continue;
        float d = sig_dist(&g_slots[i].sig, sig);
        if (d < best_d) { best_d = d; best_i = (int)i; }
    }

    uint32_t id;
    if (best_i >= 0) {
        sig_ema(&g_slots[best_i].sig, sig, 0.35f);
        g_slots[best_i].hits++;
        id = g_slots[best_i].id;
        *match_out = 1.0f - (best_d / ENTITY_MATCH_MAX_DIST);
        if (*match_out < 0.0f) *match_out = 0.0f;
        if (*match_out > 1.0f) *match_out = 1.0f;
        if ((g_slots[best_i].hits % 8u) == 0u) g_dirty = true;
    } else {
        int slot = -1;
        uint32_t oldest_hits = UINT32_MAX;
        int oldest_i = 0;
        for (uint32_t i = 0; i < ENTITY_STORE_MAX; i++) {
            if (!g_slots[i].used) { slot = (int)i; break; }
            if (g_slots[i].hits <= oldest_hits) {
                oldest_hits = g_slots[i].hits;
                oldest_i = (int)i;
            }
        }
        if (slot < 0) slot = oldest_i;
        id = g_next_id++;
        if (g_next_id == 0) g_next_id = 1;
        g_slots[slot].used = 1;
        g_slots[slot].id = id;
        g_slots[slot].cls = (uint32_t)cls;
        g_slots[slot].hits = 1;
        g_slots[slot].sig = *sig;
        *match_out = 0.0f;
        g_dirty = true;
        recount();
    }
    // Never flash here — RAM only. entity_store_poll() persists later.
    return id;
}

void entity_store_poll(bool usb_audio_idle) {
    if (!usb_audio_idle) return;
    if (g_importing) return;

    if (g_phase == SAVE_IDLE) {
        if (!g_dirty) return;
        build_stage_blob();
        g_save_slot = 1u - g_active_slot;
        g_prog_off = 0;
        g_phase = SAVE_ERASE;
    }

    if (g_phase == SAVE_ERASE) {
        flash_op_t op = {
            .off = (g_save_slot == 0u) ? ENTITY_SLOT0_OFF : ENTITY_SLOT1_OFF,
            .len = FLASH_SECTOR_SIZE
        };
        int rc = flash_safe_execute(flash_erase_cb, &op, 200);
        if (rc != PICO_OK) return; // retry next idle poll
        g_phase = SAVE_PROG;
        g_prog_off = 0;
        return; // one flash op per poll
    }

    if (g_phase == SAVE_PROG) {
        uint32_t base = (g_save_slot == 0u) ? ENTITY_SLOT0_OFF : ENTITY_SLOT1_OFF;
        uint32_t need = (sizeof(entity_blob_t) + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (g_prog_off >= need) {
            // Commit: new slot is valid; update RAM bookkeeping.
            entity_blob_t *b = (entity_blob_t *)g_stage;
            g_seq = b->seq;
            g_active_slot = g_save_slot;
            g_dirty = false;
            g_phase = SAVE_IDLE;
            uint32_t lock = dbg_line_lock();
            dbg_puts("FLASH: ACID commit seq=");
            dbg_putu32(g_seq);
            dbg_puts(" slot=");
            dbg_putu32(g_active_slot);
            dbg_puts(" n=");
            dbg_putu32(g_count);
            dbg_putc('\n');
            dbg_line_unlock(lock);
            return;
        }
        flash_prog_t op = {
            .flash_off = base + g_prog_off,
            .stage_off = g_prog_off,
            .len = FLASH_PAGE_SIZE
        };
        int rc = flash_safe_execute(flash_prog_page_cb, &op, 200);
        if (rc != PICO_OK) return;
        g_prog_off += FLASH_PAGE_SIZE;
        // one page per poll
    }
}

void entity_store_export_uart(void) {
    build_stage_blob(); // export current RAM (seq+1 preview OK for transfer)
    // Re-stamp seq as current for export fidelity
    entity_blob_t *b = (entity_blob_t *)g_stage;
    b->seq = g_seq;
    b->crc = entity_blob_crc(b);

    uint32_t nbytes = (uint32_t)sizeof(entity_blob_t);
    uint32_t lock = dbg_line_lock();
    dbg_puts("ENTBLOB BEGIN bytes=");
    dbg_putu32(nbytes);
    dbg_puts(" crc=");
    dbg_puthex32(b->crc);
    dbg_putc('\n');
    const uint8_t *p = (const uint8_t *)b;
    for (uint32_t i = 0; i < nbytes; i++) {
        if ((i & 15u) == 0u) {
            if (i) dbg_putc('\n');
            dbg_puts("ENTHEX ");
        } else {
            dbg_putc(' ');
        }
        dbg_puthex8(p[i]);
    }
    dbg_putc('\n');
    dbg_puts("ENTBLOB END\n");
    dbg_line_unlock(lock);
}

bool entity_store_import_begin(void) {
    if (g_phase != SAVE_IDLE) return false;
    memset(&g_import_blob, 0, sizeof(g_import_blob));
    g_import_pos = 0;
    g_importing = true;
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool entity_store_import_hex_line(const char *line) {
    if (!g_importing) return false;
    // Accept "ENTHEX aa bb cc ..." or raw hex bytes.
    if (strncmp(line, "ENTHEX", 6) == 0) line += 6;
    uint8_t *dst = (uint8_t *)&g_import_blob;
    while (*line) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        int hi = hex_nibble(*line++);
        if (hi < 0 || !*line) return false;
        int lo = hex_nibble(*line++);
        if (lo < 0) return false;
        if (g_import_pos >= sizeof(g_import_blob)) return false;
        dst[g_import_pos++] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool entity_store_import_end(void) {
    if (!g_importing) return false;
    g_importing = false;
    if (g_import_pos < sizeof(entity_blob_t)) {
        dbg_puts("ENT IMPORT ERR: short blob\n");
        return false;
    }
    if (!slot_valid(&g_import_blob)) {
        dbg_puts("ENT IMPORT ERR: bad magic/crc/version\n");
        return false;
    }
    memcpy(g_slots, g_import_blob.slots, sizeof(g_slots));
    g_next_id = g_import_blob.next_id ? g_import_blob.next_id : 1;
    // Keep g_seq; next opportunistic save will write seq+1 (ACID).
    recount();
    g_dirty = true;
    dbg_puts("ENT IMPORT OK n=");
    dbg_putu32(g_count);
    dbg_puts(" (RAM; flash pending idle)\n");
    return true;
}

uint32_t entity_store_count(void) { return g_count; }
uint32_t entity_store_next_id(void) { return g_next_id; }

const entity_slot_t *entity_store_slot(uint32_t index) {
    if (index >= ENTITY_STORE_MAX) return NULL;
    return &g_slots[index];
}
