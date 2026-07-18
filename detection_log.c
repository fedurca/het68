// detection_log.c — RAM-first detection events with opportunistic ACID flash.
//
// Flash layout (below entity gallery slots):
//   slot0 = PICO_FLASH_SIZE_BYTES - 4*FLASH_SECTOR_SIZE
//   slot1 = PICO_FLASH_SIZE_BYTES - 3*FLASH_SECTOR_SIZE
#include "detection_log.h"
#include "het68_time.h"
#include "debug_io.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <string.h>
#include <math.h>
#include <ctype.h>

#define DET_MAGIC   0x4C383648u  /* 'H68L' LE — Log */
#define DET_VERSION 1u
#define DET_GATE_DEG 35.0f
#define DET_MERGE_MAX_GAP_MS 120000u  // 2 min — beyond this, open a new event

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must be defined by the board"
#endif

#define DET_SLOT0_OFF  (PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE)
#define DET_SLOT1_OFF  (PICO_FLASH_SIZE_BYTES - 3u * FLASH_SECTOR_SIZE)

static det_slot_t g_slots[DET_LOG_MAX];
static uint32_t g_next_id = 1;
static uint32_t g_count = 0;
static uint32_t g_seq = 0;
static uint32_t g_active_slot = 0;
static volatile bool g_dirty = false;

typedef enum { SAVE_IDLE = 0, SAVE_ERASE, SAVE_PROG } save_phase_t;
static save_phase_t g_phase = SAVE_IDLE;
static uint32_t g_save_slot = 0;
static uint32_t g_prog_off = 0;
static uint8_t g_stage[FLASH_SECTOR_SIZE];
static det_blob_t g_import_blob;
static uint32_t g_import_pos = 0;
static bool g_importing = false;

static void put_f1(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 10.0f + 0.5f);
    if (fp >= 10u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    dbg_putu32(fp);
}

const char *det_class_name(det_class_t c) {
    switch (c) {
        case DET_WIND:     return "wind";
        case DET_DRONE:    return "drone";
        case DET_VEHICLE:  return "vehicle";
        case DET_ICE:      return "ice";
        case DET_EV:       return "ev";
        case DET_WALKER:   return "walker";
        case DET_HUMAN:    return "human";
        case DET_CAT:      return "cat";
        case DET_DOG:      return "dog";
        case DET_BIRD:     return "bird";
        case DET_SONGBIRD: return "songbird";
        case DET_CORVID:   return "corvid";
        default:           return "none";
    }
}

det_class_t det_class_from_name(const char *name) {
    if (!name) return DET_NONE;
    for (int c = 1; c <= (int)DET_CORVID; c++) {
        if (strcmp(name, det_class_name((det_class_t)c)) == 0)
            return (det_class_t)c;
    }
    return DET_NONE;
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

static uint32_t blob_crc(const det_blob_t *b) {
    det_blob_t tmp = *b;
    tmp.crc = 0;
    return crc32_update(0, (const uint8_t *)&tmp, sizeof(tmp));
}

static void recount(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < DET_LOG_MAX; i++)
        if (g_slots[i].used) n++;
    g_count = n;
}

static bool slot_valid(const det_blob_t *b) {
    if (b->magic != DET_MAGIC) return false;
    if (b->version != DET_VERSION) return false;
    if (b->count > DET_LOG_MAX) return false;
    return blob_crc(b) == b->crc;
}

static float ang_diff_deg(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return fabsf(d);
}

static void build_stage_blob(void) {
    memset(g_stage, 0xFF, sizeof(g_stage));
    det_blob_t *b = (det_blob_t *)g_stage;
    b->magic = DET_MAGIC;
    b->version = DET_VERSION;
    b->seq = g_seq + 1u;
    b->next_id = g_next_id;
    b->count = g_count;
    memcpy(b->slots, g_slots, sizeof(g_slots));
    b->crc = blob_crc(b);
}

typedef struct { uint32_t off; uint32_t len; } flash_op_t;
typedef struct { uint32_t flash_off; uint32_t stage_off; uint32_t len; } flash_prog_t;

static void __not_in_flash_func(flash_erase_cb)(void *arg) {
    flash_op_t *op = (flash_op_t *)arg;
    flash_range_erase(op->off, op->len);
}

static void __not_in_flash_func(flash_prog_page_cb)(void *arg) {
    flash_prog_t *op = (flash_prog_t *)arg;
    flash_range_program(op->flash_off, g_stage + op->stage_off, op->len);
}

void detection_log_core_init(void) {
    // flash_safe_execute needs both cores cooperating; entity_store already enables.
}

void detection_log_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    g_next_id = 1;
    g_count = 0;
    g_seq = 0;
    g_active_slot = 0;
    g_dirty = false;
    g_phase = SAVE_IDLE;
    g_importing = false;

    const det_blob_t *best = NULL;
    uint32_t best_seq = 0;
    uint32_t best_slot = 0;
    for (uint32_t s = 0; s < 2; s++) {
        uint32_t off = (s == 0u) ? DET_SLOT0_OFF : DET_SLOT1_OFF;
        const det_blob_t *b = (const det_blob_t *)(XIP_BASE + off);
        if (!slot_valid(b)) continue;
        if (!best || b->seq >= best_seq) {
            best = b;
            best_seq = b->seq;
            best_slot = s;
        }
    }
    if (best) {
        memcpy(g_slots, best->slots, sizeof(g_slots));
        g_next_id = best->next_id ? best->next_id : 1;
        g_seq = best->seq;
        g_active_slot = best_slot;
        recount();
    }
}

bool detection_log_dirty(void) { return g_dirty; }
bool detection_log_saving(void) { return g_phase != SAVE_IDLE; }
uint32_t detection_log_count(void) { return g_count; }

const det_slot_t *detection_log_slot(uint32_t index) {
    if (index >= DET_LOG_MAX) return NULL;
    return &g_slots[index];
}

uint32_t detection_log_observe(det_class_t cls, uint32_t entity_id,
                               float az, float el, float intensity_db, float conf) {
    if (cls == DET_NONE) return 0;
    if (!het68_time_synced()) return 0; // timestamps required — skip until TIME SYNC

    uint32_t now = het68_time_epoch_sec();
    uint32_t now_ms = het68_time_epoch_ms();

    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < DET_LOG_MAX; i++) {
        if (!g_slots[i].used) continue;
        if (g_slots[i].cls != (uint32_t)cls) continue;
        if (entity_id && g_slots[i].entity_id && g_slots[i].entity_id != entity_id) continue;
        float d = ang_diff_deg(az, g_slots[i].az) + fabsf(el - g_slots[i].el);
        if (d < best_d) { best_d = d; best = i; }
    }

    if (best >= 0 && best_d <= DET_GATE_DEG) {
        uint32_t prev_ms = g_slots[best].last_seen * 1000u;
        uint32_t gap_ms = (now_ms > prev_ms) ? (now_ms - prev_ms) : 0u;
        if (gap_ms > DET_MERGE_MAX_GAP_MS) {
            best = -1; // too stale — new event
        } else {
            if (gap_ms > g_slots[best].max_gap_ms)
                g_slots[best].max_gap_ms = gap_ms;
            g_slots[best].last_seen = now;
            g_slots[best].occurrence++;
            g_slots[best].az = 0.7f * g_slots[best].az + 0.3f * az;
            g_slots[best].el = 0.7f * g_slots[best].el + 0.3f * el;
            if (intensity_db > g_slots[best].intensity_db)
                g_slots[best].intensity_db = intensity_db;
            g_slots[best].conf = 0.6f * g_slots[best].conf + 0.4f * conf;
            if (entity_id) g_slots[best].entity_id = entity_id;
            g_dirty = true;
            return g_slots[best].id;
        }
    }

    int free_i = -1;
    int oldest = -1;
    uint32_t oldest_last = 0xFFFFFFFFu;
    for (int i = 0; i < DET_LOG_MAX; i++) {
        if (!g_slots[i].used) { free_i = i; break; }
        if (g_slots[i].last_seen < oldest_last) {
            oldest_last = g_slots[i].last_seen;
            oldest = i;
        }
    }
    int slot = (free_i >= 0) ? free_i : oldest;
    if (slot < 0) return 0;

    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    g_slots[slot].used = 1;
    g_slots[slot].id = id;
    g_slots[slot].cls = (uint32_t)cls;
    g_slots[slot].entity_id = entity_id;
    g_slots[slot].first_seen = now;
    g_slots[slot].last_seen = now;
    g_slots[slot].occurrence = 1;
    g_slots[slot].max_gap_ms = 0;
    g_slots[slot].az = az;
    g_slots[slot].el = el;
    g_slots[slot].intensity_db = intensity_db;
    g_slots[slot].conf = conf;
    recount();
    g_dirty = true;
    return id;
}

bool detection_log_delete_id(uint32_t id) {
    for (int i = 0; i < DET_LOG_MAX; i++) {
        if (g_slots[i].used && g_slots[i].id == id) {
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            recount();
            g_dirty = true;
            return true;
        }
    }
    return false;
}

void detection_log_clear(void) {
    memset(g_slots, 0, sizeof(g_slots));
    g_count = 0;
    g_dirty = true;
}

void detection_log_poll(bool usb_audio_idle) {
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
            .off = (g_save_slot == 0u) ? DET_SLOT0_OFF : DET_SLOT1_OFF,
            .len = FLASH_SECTOR_SIZE
        };
        int rc = flash_safe_execute(flash_erase_cb, &op, 200);
        if (rc != PICO_OK) return;
        g_phase = SAVE_PROG;
        g_prog_off = 0;
        return;
    }

    if (g_phase == SAVE_PROG) {
        uint32_t base = (g_save_slot == 0u) ? DET_SLOT0_OFF : DET_SLOT1_OFF;
        uint32_t need = (sizeof(det_blob_t) + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
        if (g_prog_off >= need) {
            det_blob_t *b = (det_blob_t *)g_stage;
            g_seq = b->seq;
            g_active_slot = g_save_slot;
            g_dirty = false;
            g_phase = SAVE_IDLE;
            uint32_t lock = dbg_line_lock();
            dbg_puts("FLASH: DET ACID commit seq=");
            dbg_putu32(g_seq);
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
    }
}

static void print_slot_line(const det_slot_t *s) {
    dbg_puts("DET id=");
    dbg_putu32(s->id);
    dbg_puts(" class=");
    dbg_puts(det_class_name((det_class_t)s->cls));
    dbg_puts(" entity=");
    dbg_putu32(s->entity_id);
    dbg_puts(" first=");
    dbg_putu32(s->first_seen);
    dbg_puts(" last=");
    dbg_putu32(s->last_seen);
    dbg_puts(" occ=");
    dbg_putu32(s->occurrence);
    dbg_puts(" max_gap_ms=");
    dbg_putu32(s->max_gap_ms);
    dbg_puts(" az="); put_f1(s->az);
    dbg_puts(" el="); put_f1(s->el);
    dbg_puts(" inten="); put_f1(s->intensity_db);
    dbg_puts("dB conf="); put_f1(s->conf);
    dbg_putc('\n');
}

void detection_log_list_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== detection log n=");
    dbg_putu32(g_count);
    dbg_puts(" next_id=");
    dbg_putu32(g_next_id);
    dbg_puts(" time_synced=");
    dbg_puts(het68_time_synced() ? "1" : "0");
    dbg_puts(" ===\n");
    for (uint32_t i = 0; i < DET_LOG_MAX; i++) {
        if (g_slots[i].used) print_slot_line(&g_slots[i]);
    }
    dbg_puts("=== end detection log ===\n");
    dbg_line_unlock(lock);
}

void detection_log_export_nvr(void) {
    // JSON Lines — one object per detection for smart-NVR event correlation.
    uint32_t lock = dbg_line_lock();
    dbg_puts("NVREVT BEGIN device=het68 v=1\n");
    for (uint32_t i = 0; i < DET_LOG_MAX; i++) {
        const det_slot_t *s = &g_slots[i];
        if (!s->used) continue;
        dbg_puts("{\"v\":1,\"device\":\"het68\",\"type\":\"detection\",\"id\":");
        dbg_putu32(s->id);
        dbg_puts(",\"class\":\"");
        dbg_puts(det_class_name((det_class_t)s->cls));
        dbg_puts("\",\"entity_id\":");
        dbg_putu32(s->entity_id);
        dbg_puts(",\"first_seen\":");
        dbg_putu32(s->first_seen);
        dbg_puts(",\"last_seen\":");
        dbg_putu32(s->last_seen);
        dbg_puts(",\"occurrence\":");
        dbg_putu32(s->occurrence);
        dbg_puts(",\"max_gap_ms\":");
        dbg_putu32(s->max_gap_ms);
        dbg_puts(",\"az\":"); put_f1(s->az);
        dbg_puts(",\"el\":"); put_f1(s->el);
        dbg_puts(",\"intensity_db\":"); put_f1(s->intensity_db);
        dbg_puts(",\"conf\":"); put_f1(s->conf);
        dbg_puts("}\n");
    }
    dbg_puts("NVREVT END\n");
    dbg_line_unlock(lock);
}

void detection_log_export_hex(void) {
    build_stage_blob();
    det_blob_t *b = (det_blob_t *)g_stage;
    b->seq = g_seq;
    b->crc = blob_crc(b);

    uint32_t nbytes = (uint32_t)sizeof(det_blob_t);
    uint32_t lock = dbg_line_lock();
    dbg_puts("DETBLOB BEGIN bytes=");
    dbg_putu32(nbytes);
    dbg_puts(" crc=");
    dbg_puthex32(b->crc);
    dbg_putc('\n');
    const uint8_t *p = (const uint8_t *)b;
    for (uint32_t i = 0; i < nbytes; i++) {
        if ((i & 15u) == 0u) {
            if (i) dbg_putc('\n');
            dbg_puts("DETHEX ");
        } else {
            dbg_putc(' ');
        }
        dbg_puthex8(p[i]);
    }
    dbg_putc('\n');
    dbg_puts("DETBLOB END\n");
    dbg_line_unlock(lock);
}

bool detection_log_import_begin(void) {
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

bool detection_log_import_hex_line(const char *line) {
    if (!g_importing) return false;
    if (strncmp(line, "DETHEX", 6) == 0) line += 6;
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

bool detection_log_import_end(void) {
    if (!g_importing) return false;
    g_importing = false;
    if (g_import_pos < sizeof(det_blob_t)) {
        dbg_puts("DET IMPORT ERR: short blob\n");
        return false;
    }
    if (!slot_valid(&g_import_blob)) {
        dbg_puts("DET IMPORT ERR: bad magic/crc/version\n");
        return false;
    }
    memcpy(g_slots, g_import_blob.slots, sizeof(g_slots));
    g_next_id = g_import_blob.next_id ? g_import_blob.next_id : 1;
    recount();
    g_dirty = true;
    dbg_puts("DET IMPORT OK n=");
    dbg_putu32(g_count);
    dbg_puts(" (RAM; flash pending idle)\n");
    return true;
}
