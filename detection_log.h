// detection_log.h — event log separate from entity gallery (NVR correlation).
//
// Timestamps (first_seen / last_seen) are recorded only after TIME SYNC since boot.
// Flash: ACID dual-slot in the two sectors just below the entity gallery.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DET_LOG_MAX 32

// Detection classes (orthogonal to entity gallery templates).
typedef enum {
    DET_NONE     = 0,
    DET_WIND     = 1,
    DET_DRONE    = 2,
    DET_VEHICLE  = 3,
    DET_ICE      = 4,
    DET_EV       = 5,
    DET_WALKER   = 6,
    DET_HUMAN    = 7,
    DET_CAT      = 8,
    DET_DOG      = 9,
    DET_BIRD     = 10,
    DET_SONGBIRD = 11,
    DET_CORVID   = 12,
} det_class_t;

typedef struct {
    uint32_t used;
    uint32_t id;
    uint32_t cls;
    uint32_t entity_id;     // optional gallery link (0 = none)
    uint32_t first_seen;    // unix epoch seconds
    uint32_t last_seen;     // unix epoch seconds
    uint32_t occurrence;    // times refreshed / seen
    uint32_t max_gap_ms;    // largest gap between consecutive observations
    float az;
    float el;
    float intensity_db;     // peak intensity (dBFS-ish)
    float conf;
} det_slot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t next_id;
    uint32_t count;
    uint32_t crc;
    uint32_t reserved[2];
    det_slot_t slots[DET_LOG_MAX];
} det_blob_t;

void detection_log_core_init(void);
void detection_log_init(void);
void detection_log_poll(bool usb_audio_idle);

bool detection_log_dirty(void);
bool detection_log_saving(void);

// Observe a live detection. No-op (returns 0) until TIME SYNC since boot.
// Merges into an existing slot when class (+ optional entity) and angle match.
uint32_t detection_log_observe(det_class_t cls, uint32_t entity_id,
                               float az, float el, float intensity_db, float conf);

uint32_t detection_log_count(void);
const det_slot_t *detection_log_slot(uint32_t index);
bool detection_log_delete_id(uint32_t id);
void detection_log_clear(void);

void detection_log_list_uart(void);
void detection_log_export_nvr(void);   // JSON Lines for smart NVR
void detection_log_export_hex(void);   // DETBLOB hex for backup/restore
bool detection_log_import_begin(void);
bool detection_log_import_hex_line(const char *line);
bool detection_log_import_end(void);

const char *det_class_name(det_class_t c);
det_class_t det_class_from_name(const char *name);
