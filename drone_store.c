// drone_store.c — RAM-first known-drone registry with opportunistic ACID flash.
//
// Flash layout (below BTstack TLV / DET / ENT):
//   slot0 = PICO_FLASH_SIZE_BYTES - 8*FLASH_SECTOR_SIZE
//   slot1 = PICO_FLASH_SIZE_BYTES - 7*FLASH_SECTOR_SIZE
// Full layout end→start: ENT[-2,-1], DET[-4,-3], BT[-6,-5], DRONE[-8,-7].
#include "drone_store.h"
#include "debug_io.h"
#include "het68_time.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <string.h>

#define DRONE_MAGIC   0x44383648u  /* 'H68D' LE */
#define DRONE_VERSION 1u

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must be defined by the board"
#endif

#define DRONE_SLOT0_OFF  (PICO_FLASH_SIZE_BYTES - 8u * FLASH_SECTOR_SIZE)
#define DRONE_SLOT1_OFF  (PICO_FLASH_SIZE_BYTES - 7u * FLASH_SECTOR_SIZE)

_Static_assert(sizeof(drone_blob_t) <= FLASH_SECTOR_SIZE,
               "drone_blob_t must fit in one flash sector");

#define DRONE_FLAG_BASIC    0x01u
#define DRONE_FLAG_LOCATION 0x02u

static drone_slot_t g_slots[DRONE_STORE_MAX];
static uint32_t g_next_id = 1;
static uint32_t g_count = 0;
static uint32_t g_seq = 0;
static uint32_t g_active_slot = 0;
static volatile bool g_dirty = false;

typedef enum {
    SAVE_IDLE = 0,
    SAVE_ERASE,
    SAVE_PROG
} save_phase_t;

static save_phase_t g_phase = SAVE_IDLE;
static uint32_t g_save_slot = 0;
static uint32_t g_prog_off = 0;
static uint8_t g_stage[FLASH_SECTOR_SIZE];

static void put_i32(int32_t v) {
    if (v < 0) {
        dbg_putc('-');
        dbg_putu32((uint32_t)(-v));
    } else {
        dbg_putu32((uint32_t)v);
    }
}

static void put_mac(const uint8_t addr[6]) {
    for (int b = 0; b < 6; b++) {
        dbg_puthex8(addr[b]);
        if (b < 5) dbg_putc(':');
    }
}

uint8_t drone_rssi_quality(int8_t rssi) {
    // Map typical BLE RSSI: -40 dBm → 100, -90 dBm → 20, clamp.
    int q = 100 + ((int)rssi + 40) * 2; // -40→100, -90→0 before floor
    if (q > 100) q = 100;
    if (q < 20) q = 20;
    if (rssi < -95) q = 10;
    return (uint8_t)q;
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

uint32_t drone_blob_crc(const drone_blob_t *b) {
    drone_blob_t tmp = *b;
    tmp.crc = 0;
    return crc32_update(0, (const uint8_t *)&tmp, sizeof(tmp));
}

static void recount(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < DRONE_STORE_MAX; i++)
        if (g_slots[i].used) n++;
    g_count = n;
}

static bool slot_valid(const drone_blob_t *b) {
    if (b->magic != DRONE_MAGIC) return false;
    if (b->version != DRONE_VERSION) return false;
    if (b->count > DRONE_STORE_MAX) return false;
    return drone_blob_crc(b) == b->crc;
}

static void load_from_flash(void) {
    const drone_blob_t *s0 = (const drone_blob_t *)(XIP_BASE + DRONE_SLOT0_OFF);
    const drone_blob_t *s1 = (const drone_blob_t *)(XIP_BASE + DRONE_SLOT1_OFF);
    memset(g_slots, 0, sizeof(g_slots));
    g_next_id = 1;
    g_count = 0;
    g_seq = 0;
    g_active_slot = 0;

    bool v0 = slot_valid(s0);
    bool v1 = slot_valid(s1);
    const drone_blob_t *best = NULL;
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
    drone_blob_t *b = (drone_blob_t *)g_stage;
    memset(b, 0, sizeof(*b));
    b->magic = DRONE_MAGIC;
    b->version = DRONE_VERSION;
    b->seq = g_seq + 1u;
    b->next_id = g_next_id;
    recount();
    b->count = g_count;
    memcpy(b->slots, g_slots, sizeof(g_slots));
    b->crc = drone_blob_crc(b);
}

typedef struct { uint32_t off; uint32_t len; } flash_op_t;

static void __not_in_flash_func(flash_erase_cb)(void *param) {
    flash_op_t *op = (flash_op_t *)param;
    flash_range_erase(op->off, FLASH_SECTOR_SIZE);
}

typedef struct {
    uint32_t flash_off;
    uint32_t stage_off;
    uint32_t len;
} flash_prog_t;

static void __not_in_flash_func(flash_prog_page_cb)(void *param) {
    flash_prog_t *op = (flash_prog_t *)param;
    flash_range_program(op->flash_off, &g_stage[op->stage_off], op->len);
}

void drone_store_core_init(void) {
    // flash_safe_execute_core_init is shared; entity_store already calls it.
}

void drone_store_init(void) {
    load_from_flash();
    g_dirty = false;
    g_phase = SAVE_IDLE;
}

bool drone_store_dirty(void) { return g_dirty; }
bool drone_store_saving(void) { return g_phase != SAVE_IDLE; }
uint32_t drone_store_count(void) { return g_count; }
uint32_t drone_store_seq(void) { return g_seq; }

const drone_slot_t *drone_store_slot(uint32_t index) {
    if (index >= DRONE_STORE_MAX) return NULL;
    if (!g_slots[index].used) return NULL;
    return &g_slots[index];
}

static int find_by_addr(const uint8_t addr[6]) {
    for (uint32_t i = 0; i < DRONE_STORE_MAX; i++) {
        if (!g_slots[i].used) continue;
        if (memcmp(g_slots[i].addr, addr, 6) == 0) return (int)i;
    }
    return -1;
}

static int find_by_uas(const char *uas_id) {
    if (!uas_id || !uas_id[0]) return -1;
    for (uint32_t i = 0; i < DRONE_STORE_MAX; i++) {
        if (!g_slots[i].used) continue;
        if (g_slots[i].uas_id[0] == '\0') continue;
        if (strncmp(g_slots[i].uas_id, uas_id, DRONE_UAS_ID_LEN) == 0)
            return (int)i;
    }
    return -1;
}

static int alloc_slot(void) {
    int free_i = -1;
    int oldest_i = 0;
    uint32_t oldest_last = UINT32_MAX;
    uint32_t oldest_hits = UINT32_MAX;
    for (uint32_t i = 0; i < DRONE_STORE_MAX; i++) {
        if (!g_slots[i].used) {
            if (free_i < 0) free_i = (int)i;
            continue;
        }
        uint32_t last = g_slots[i].last_seen ? g_slots[i].last_seen : 0;
        if (last < oldest_last ||
            (last == oldest_last && g_slots[i].hits < oldest_hits)) {
            oldest_last = last;
            oldest_hits = g_slots[i].hits;
            oldest_i = (int)i;
        }
    }
    return (free_i >= 0) ? free_i : oldest_i;
}

void drone_store_observe(const uint8_t addr[6], int8_t rssi,
                         const char *uas_id, uint8_t id_type, uint8_t ua_type,
                         bool has_basic, bool has_location,
                         int32_t lat_e7, int32_t lon_e7, int16_t alt_geoid_m,
                         uint16_t speed_cm_s, uint16_t heading_deg,
                         bool time_offer) {
    if (!addr) return;

    int idx = find_by_addr(addr);
    if (idx < 0 && has_basic && uas_id && uas_id[0])
        idx = find_by_uas(uas_id);

    bool is_new = false;
    if (idx < 0) {
        idx = alloc_slot();
        memset(&g_slots[idx], 0, sizeof(g_slots[idx]));
        g_slots[idx].used = 1;
        memcpy(g_slots[idx].addr, addr, 6);
        g_slots[idx].heading_deg = 361;
        is_new = true;
        g_next_id++;
        if (g_next_id == 0) g_next_id = 1;
    }

    drone_slot_t *s = &g_slots[idx];
    memcpy(s->addr, addr, 6);
    s->rssi = rssi;
    s->quality = drone_rssi_quality(rssi);
    if (has_basic && uas_id) {
        strncpy(s->uas_id, uas_id, DRONE_UAS_ID_LEN - 1);
        s->uas_id[DRONE_UAS_ID_LEN - 1] = '\0';
        s->id_type = id_type;
        s->ua_type = ua_type;
        s->flags |= DRONE_FLAG_BASIC;
    }
    if (has_location) {
        s->lat_e7 = lat_e7;
        s->lon_e7 = lon_e7;
        s->alt_geoid_m = alt_geoid_m;
        s->speed_cm_s = speed_cm_s;
        s->heading_deg = heading_deg;
        s->flags |= DRONE_FLAG_LOCATION;
    }
    if (time_offer) s->time_offers++;

    s->hits++;
    if (het68_time_synced()) {
        uint32_t now = het68_time_epoch_sec();
        if (!s->first_seen) s->first_seen = now;
        s->last_seen = now;
    }

    recount();
    if (is_new || time_offer || has_location || (s->hits % 8u) == 0u)
        g_dirty = true;
}

void drone_store_poll(bool usb_audio_idle) {
    if (!usb_audio_idle) return;

    if (g_phase == SAVE_IDLE) {
        if (!g_dirty) return;
        build_stage_blob();
        g_save_slot = 1u - g_active_slot;
        g_prog_off = 0;
        g_phase = SAVE_ERASE;
    }

    if (g_phase == SAVE_ERASE) {
        flash_op_t op = {
            .off = (g_save_slot == 0u) ? DRONE_SLOT0_OFF : DRONE_SLOT1_OFF,
            .len = FLASH_SECTOR_SIZE
        };
        int rc = flash_safe_execute(flash_erase_cb, &op, 200);
        if (rc != PICO_OK) return;
        g_phase = SAVE_PROG;
        g_prog_off = 0;
        return;
    }

    if (g_phase == SAVE_PROG) {
        uint32_t base = (g_save_slot == 0u) ? DRONE_SLOT0_OFF : DRONE_SLOT1_OFF;
        uint32_t need = (sizeof(drone_blob_t) + FLASH_PAGE_SIZE - 1u) &
                        ~(FLASH_PAGE_SIZE - 1u);
        if (g_prog_off >= need) {
            drone_blob_t *b = (drone_blob_t *)g_stage;
            g_seq = b->seq;
            g_active_slot = g_save_slot;
            g_dirty = false;
            g_phase = SAVE_IDLE;
            uint32_t lock = dbg_line_lock();
            dbg_puts("FLASH: DRONE ACID commit seq=");
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
    }
}

static void dump_one(const drone_slot_t *s) {
    dbg_puts("DRONE id=");
    // synthetic stable-ish id: first 3 MAC bytes + hits not needed; use index via uas
    dbg_puts(s->uas_id[0] ? s->uas_id : "(no-id)");
    dbg_puts(" mac=");
    put_mac(s->addr);
    dbg_puts(" rssi=");
    put_i32(s->rssi);
    dbg_puts(" q=");
    dbg_putu32(s->quality);
    dbg_puts(" hits=");
    dbg_putu32(s->hits);
    dbg_puts(" time_offers=");
    dbg_putu32(s->time_offers);
    if (s->flags & DRONE_FLAG_LOCATION) {
        dbg_puts(" lat=");
        put_i32(s->lat_e7);
        dbg_puts(" lon=");
        put_i32(s->lon_e7);
        dbg_puts(" alt_m=");
        put_i32(s->alt_geoid_m);
    }
    if (s->first_seen || s->last_seen) {
        dbg_puts(" first=");
        dbg_putu32(s->first_seen);
        dbg_puts(" last=");
        dbg_putu32(s->last_seen);
    }
    dbg_puts(" ua_type=");
    dbg_putu32(s->ua_type);
    dbg_putc('\n');
}

void drone_store_stats_uart(void) {
    dbg_puts("DRONE store n=");
    dbg_putu32(g_count);
    dbg_puts(" seq=");
    dbg_putu32(g_seq);
    dbg_puts(" flash_slot=");
    dbg_putu32(g_active_slot);
    if (g_dirty) dbg_puts(" dirty");
    if (g_phase != SAVE_IDLE) dbg_puts(" saving");
    dbg_putc('\n');
}

void drone_store_list_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== known drones (flash");
    if (g_dirty) dbg_puts("+dirty");
    dbg_puts(") ===\n");
    drone_store_stats_uart();
    if (g_count == 0) dbg_puts("(empty)\n");
    for (uint32_t i = 0; i < DRONE_STORE_MAX; i++) {
        if (!g_slots[i].used) continue;
        dump_one(&g_slots[i]);
    }
    dbg_puts("=== end known drones ===\n");
    dbg_line_unlock(lock);
}

bool drone_store_clear(void) {
    if (g_phase != SAVE_IDLE) return false;
    memset(g_slots, 0, sizeof(g_slots));
    g_count = 0;
    g_next_id = 1;
    g_dirty = true;
    return true;
}
