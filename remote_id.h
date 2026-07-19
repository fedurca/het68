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
    uint32_t last_ms;
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
