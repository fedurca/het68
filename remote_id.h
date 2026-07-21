// remote_id.h — OpenDroneID / EU Direct Remote ID BLE scanner (ASTM F3411).
//
// Active only on CYW43 boards (Wi-Fi/BT). On pico2 builds all APIs are stubs.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RID_MAX_TRACKS 8
#define RID_UAS_ID_LEN 21  // 20 chars + NUL

typedef struct {
    uint32_t used;
    uint8_t  addr[6];
    int8_t   rssi;
    char     uas_id[RID_UAS_ID_LEN];
    uint8_t  id_type;       // ASTM ID type nibble
    uint8_t  ua_type;       // ASTM UA type nibble
    uint8_t  has_basic;
    uint8_t  has_location;
    int32_t  lat_e7;        // deg * 1e7
    int32_t  lon_e7;
    int16_t  alt_geoid_m;   // m, ASTM encoding decoded to metres
    uint16_t speed_cm_s;    // horizontal, approx cm/s
    uint16_t heading_deg;   // 0..359, 361 = unknown
    int32_t  op_lat_e7;     // operator / takeoff from System msg
    int32_t  op_lon_e7;
    uint8_t  has_operator;
    uint32_t last_ms;
    uint32_t flash_ms;      // last_ms already persisted to drone_store
    uint8_t  sys_pending;   // System msg seen since last main-loop observe
} rid_track_t;

// Returns true if BT RID is compiled in and init succeeded.
bool remote_id_init(void);
bool remote_id_available(void);

// Enable/disable scanning (default ON after successful init).
void remote_id_set_enabled(bool on);
bool remote_id_enabled(void);

// Age out stale tracks; safe to call from main loop (non-blocking).
void remote_id_poll(void);

uint32_t remote_id_count(void);
const rid_track_t *remote_id_track(uint32_t index);
void remote_id_list_uart(void);

// Heartbeat counter: distinct UAS IDs / MACs seen in the last stale window.
uint32_t remote_id_active_count(void);

// Last wall-clock offered from an OpenDroneID System message (unix epoch), or 0.
uint32_t remote_id_last_unix(void);
// How many successful TIME SYNCs were applied from RID System timestamps.
uint32_t remote_id_time_sync_count(void);

// Operator/array origin from the latest System message (lat/lon e7). False if unknown.
bool remote_id_origin_get(int32_t *lat_e7, int32_t *lon_e7);

// Convert aircraft GPS (relative to origin) → DOA-frame az/el (deg) and range (m).
// DOA frame: +X north, +Y east, az = atan2(east, north) in [0,360).
bool remote_id_aircraft_azel(const rid_track_t *t, float *az_deg, float *el_deg, float *rng_m);
