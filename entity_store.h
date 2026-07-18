// entity_store.h — flash-backed gallery of recognised acoustic entities.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ENTITY_STORE_MAX 16

typedef enum {
    ENT_NONE  = 0,
    ENT_HUMAN = 2,
    ENT_CAT   = 3,
    ENT_DOG   = 4,
    ENT_ICE   = 5,   // ICE vehicle (combustion)
    ENT_EV    = 6,   // electric vehicle
} entity_class_t;

// Compact acoustic signature used for re-identification.
typedef struct {
    float cadence_hz;  // walkers: step rate; vehicles: envelope modulation proxy
    float peak_db;
    float low_ratio;
    float high_ratio;
    float mid_ratio;   // vehicles (and unused~0 for walkers)
    float crest;
    float az_n;
    float el_n;
} entity_sig_t;

typedef struct {
    uint32_t used;     // 0/1 (uint32 for flash alignment)
    uint32_t id;
    uint32_t cls;      // entity_class_t
    uint32_t hits;
    entity_sig_t sig;
} entity_slot_t;

// Load gallery from the last flash sector (or empty if blank/invalid).
void entity_store_init(void);

// Print all persisted entities to the debug UART (call after dbg_init).
void entity_store_dump_uart(void);

// Match signature to gallery (same class) or create a new id.
// match_out: 0..1 heuristic similarity (0 = new entity).
// Persists to flash on create, and periodically on updates.
uint32_t entity_store_match_or_create(entity_class_t cls, const entity_sig_t *sig,
                                      float *match_out);

uint32_t entity_store_count(void);
uint32_t entity_store_next_id(void);
const entity_slot_t *entity_store_slot(uint32_t index);

const char *entity_class_name(entity_class_t c);

// Must be called once on each core before flash writes (pico_flash).
void entity_store_core_init(void);
