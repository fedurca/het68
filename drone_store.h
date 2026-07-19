// drone_store.h — flash-backed registry of known OpenDroneID drones.
//
// Flash: ACID dual-slot in sectors [-8,-7] (below BTstack TLV / DET / ENT).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DRONE_STORE_MAX   16
#define DRONE_UAS_ID_LEN  21  // 20 chars + NUL

typedef struct {
    uint32_t used;
    uint8_t  addr[6];
    int8_t   rssi;
    uint8_t  id_type;
    uint8_t  ua_type;
    uint8_t  flags;          // bit0 has_basic, bit1 has_location
    uint8_t  quality;        // last RSSI-derived quality 0..100
    char     uas_id[DRONE_UAS_ID_LEN];
    uint8_t  _pad0;
    int32_t  lat_e7;
    int32_t  lon_e7;
    int16_t  alt_geoid_m;
    uint16_t speed_cm_s;
    uint16_t heading_deg;
    uint16_t _pad1;
    uint32_t first_seen;     // unix epoch (0 if never synced when seen)
    uint32_t last_seen;      // unix epoch
    uint32_t hits;
    uint32_t time_offers;    // System messages that offered wall-clock
} drone_slot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t next_id;
    uint32_t count;
    uint32_t crc;
    uint32_t reserved[2];
    drone_slot_t slots[DRONE_STORE_MAX];
} drone_blob_t;

void drone_store_core_init(void);
void drone_store_init(void);
void drone_store_poll(bool usb_audio_idle);

bool drone_store_dirty(void);
bool drone_store_saving(void);

uint32_t drone_store_count(void);
uint32_t drone_store_seq(void);
const drone_slot_t *drone_store_slot(uint32_t index);

// Upsert by BLE MAC (preferred) or UAS ID. RAM only — flash via poll().
void drone_store_observe(const uint8_t addr[6], int8_t rssi,
                         const char *uas_id, uint8_t id_type, uint8_t ua_type,
                         bool has_basic, bool has_location,
                         int32_t lat_e7, int32_t lon_e7, int16_t alt_geoid_m,
                         uint16_t speed_cm_s, uint16_t heading_deg,
                         bool time_offer);

void drone_store_list_uart(void);
void drone_store_stats_uart(void);

bool drone_store_clear(void);
uint32_t drone_blob_crc(const drone_blob_t *b);

// Map RSSI (dBm) → quality 0..100 for time / drone metadata.
uint8_t drone_rssi_quality(int8_t rssi);
