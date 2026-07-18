// entity_store.c — persistent entity gallery in the last flash sector.
//
// Layout: one 4 KiB sector at (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE).
// Reads are XIP (safe anytime). Writes use flash_safe_execute() so both cores
// leave XIP; this may briefly glitch USB audio — saves are rare (new entity /
// every 8th hit update).
#include "entity_store.h"
#include "debug_io.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <string.h>
#include <math.h>

#define ENTITY_MAGIC   0x45383648u  /* 'H68E' LE */
#define ENTITY_VERSION 1u

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must be defined by the board"
#endif

#define ENTITY_FLASH_OFF  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t next_id;
    uint32_t count;
    uint32_t crc;
    uint32_t reserved[3];
    entity_slot_t slots[ENTITY_STORE_MAX];
} entity_blob_t;

static entity_slot_t g_slots[ENTITY_STORE_MAX];
static uint32_t g_next_id = 1;
static uint32_t g_count = 0;
static volatile bool g_dirty = false;
static uint32_t g_save_countdown = 0;

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
        case ENT_HUMAN: return "human";
        case ENT_CAT:   return "cat";
        case ENT_DOG:   return "dog";
        case ENT_ICE:   return "ice";
        case ENT_EV:    return "ev";
        default:        return "none";
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

static uint32_t blob_crc(const entity_blob_t *b) {
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

static void load_from_flash(void) {
    const entity_blob_t *flash = (const entity_blob_t *)(XIP_BASE + ENTITY_FLASH_OFF);
    memset(g_slots, 0, sizeof(g_slots));
    g_next_id = 1;
    g_count = 0;

    if (flash->magic != ENTITY_MAGIC || flash->version != ENTITY_VERSION) return;
    if (flash->count > ENTITY_STORE_MAX) return;
    if (blob_crc(flash) != flash->crc) return;

    memcpy(g_slots, flash->slots, sizeof(g_slots));
    g_next_id = flash->next_id ? flash->next_id : 1;
    recount();
}

typedef struct {
    entity_blob_t blob;
} save_job_t;

// RAM staging buffer for the flash write callback (must not live in XIP).
static uint8_t g_flash_pagebuf[FLASH_SECTOR_SIZE];

static void __not_in_flash_func(save_job_fn)(void *param) {
    save_job_t *job = (save_job_t *)param;
    flash_range_erase(ENTITY_FLASH_OFF, FLASH_SECTOR_SIZE);
    size_t nbytes = (sizeof(job->blob) + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
    // Fill unused bytes with erased state.
    for (size_t i = 0; i < FLASH_SECTOR_SIZE; i++) g_flash_pagebuf[i] = 0xFFu;
    // memcpy may live in flash — copy manually while XIP is offline.
    const uint8_t *src = (const uint8_t *)&job->blob;
    for (size_t i = 0; i < sizeof(job->blob); i++) g_flash_pagebuf[i] = src[i];
    flash_range_program(ENTITY_FLASH_OFF, g_flash_pagebuf, nbytes);
}

static bool persist_now(void) {
    save_job_t job;
    memset(&job, 0, sizeof(job));
    job.blob.magic = ENTITY_MAGIC;
    job.blob.version = ENTITY_VERSION;
    job.blob.next_id = g_next_id;
    recount();
    job.blob.count = g_count;
    memcpy(job.blob.slots, g_slots, sizeof(g_slots));
    job.blob.crc = blob_crc(&job.blob);

    int rc = flash_safe_execute(save_job_fn, &job, 1000);
    if (rc == PICO_OK) {
        g_dirty = false;
        return true;
    }
    return false;
}

void entity_store_core_init(void) {
    flash_safe_execute_core_init();
}

void entity_store_init(void) {
    load_from_flash();
    g_dirty = false;
}

void entity_store_dump_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== entity store (flash) n=");
    dbg_putu32(g_count);
    dbg_puts(" next_id=");
    dbg_putu32(g_next_id);
    dbg_puts(" ===\n");
    if (g_count == 0) {
        dbg_puts("(empty — no persisted entities yet)\n");
    }
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
        dbg_puts("dB\n");
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

#define ENTITY_MATCH_MAX_DIST 0.55f

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

    bool created = false;
    uint32_t id;
    if (best_i >= 0) {
        sig_ema(&g_slots[best_i].sig, sig, 0.35f);
        g_slots[best_i].hits++;
        id = g_slots[best_i].id;
        *match_out = 1.0f - (best_d / ENTITY_MATCH_MAX_DIST);
        if (*match_out < 0.0f) *match_out = 0.0f;
        if (*match_out > 1.0f) *match_out = 1.0f;
        // Persist occasionally on updates to limit flash wear.
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
        created = true;
        g_dirty = true;
        recount();
    }

    if (g_dirty) {
        // Defer a couple of loop iterations worth if called from hot path — but
        // we persist immediately here; saves are rare.
        if (!persist_now()) {
            // Keep dirty; try again later via countdown from doa if needed.
            g_save_countdown = 5;
        } else if (created) {
            uint32_t lock = dbg_line_lock();
            dbg_puts("FLASH: saved entity id=");
            dbg_putu32(id);
            dbg_puts(" class=");
            dbg_puts(entity_class_name(cls));
            dbg_putc('\n');
            dbg_line_unlock(lock);
        }
    }
    (void)g_save_countdown;
    return id;
}

uint32_t entity_store_count(void) { return g_count; }
uint32_t entity_store_next_id(void) { return g_next_id; }

const entity_slot_t *entity_store_slot(uint32_t index) {
    if (index >= ENTITY_STORE_MAX) return NULL;
    return &g_slots[index];
}
