// entity_store.h — RAM gallery + opportunistic ACID flash persistence.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ENTITY_STORE_MAX 16

typedef enum {
    ENT_NONE     = 0,
    ENT_HUMAN    = 2,
    ENT_CAT      = 3,
    ENT_DOG      = 4,
    ENT_ICE      = 5,
    ENT_EV       = 6,
    ENT_BIRD     = 7,   // generic bird
    ENT_SONGBIRD = 8,   // high/tonal chirps
    ENT_CORVID   = 9,   // harsher / lower bird calls
} entity_class_t;

typedef struct {
    float cadence_hz;
    float peak_db;
    float low_ratio;
    float high_ratio;
    float mid_ratio;
    float crest;
    float az_n;
    float el_n;
} entity_sig_t;

typedef struct {
    uint32_t used;
    uint32_t id;
    uint32_t cls;
    uint32_t hits;
    entity_sig_t sig;
} entity_slot_t;

// On-wire / flash blob header + slots (version 2).
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;       // monotonic; higher wins on load
    uint32_t next_id;
    uint32_t count;
    uint32_t crc;       // CRC32 of whole blob with crc field zeroed
    uint32_t reserved[2];
    entity_slot_t slots[ENTITY_STORE_MAX];
} entity_blob_t;

void entity_store_core_init(void);
void entity_store_init(void);                 // load best ACID slot into RAM
void entity_store_dump_uart(void);

// Match/create in RAM only — never blocks on flash.
uint32_t entity_store_match_or_create(entity_class_t cls, const entity_sig_t *sig,
                                      float *match_out);

// Opportunistic ACID save: call from core0 when USB audio is idle (alt=0).
// Performs at most one flash erase or one 256 B page program per call.
void entity_store_poll(bool usb_audio_idle);

bool entity_store_dirty(void);
bool entity_store_saving(void);

uint32_t entity_store_count(void);
uint32_t entity_store_next_id(void);
const entity_slot_t *entity_store_slot(uint32_t index);

// UART transfer helpers
void entity_store_export_uart(void);          // ENTBLOB hex dump
bool entity_store_import_begin(void);
bool entity_store_import_hex_line(const char *line); // returns false on error
bool entity_store_import_end(void);           // validate + load RAM, mark dirty

const char *entity_class_name(entity_class_t c);
uint32_t entity_blob_crc(const entity_blob_t *b);
