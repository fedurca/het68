// remote_id.c — OpenDroneID BLE advertisement scanner (EU Direct Remote ID).
//
// Parses ASTM F3411 messages from BLE Service Data UUID 0xFFFA.
// No malloc; updates static track table from the BTstack HCI callback.
#include "remote_id.h"
#include "debug_io.h"
#include "detection_log.h"
#include "pico/stdlib.h"
#include <string.h>

#ifndef HET68_BT_RID
#define HET68_BT_RID 0
#endif

#if !HET68_BT_RID

bool remote_id_init(void) { return false; }
bool remote_id_available(void) { return false; }
void remote_id_set_enabled(bool on) { (void)on; }
bool remote_id_enabled(void) { return false; }
void remote_id_poll(void) {}
uint32_t remote_id_count(void) { return 0; }
const rid_track_t *remote_id_track(uint32_t index) { (void)index; return NULL; }
void remote_id_list_uart(void) {
    dbg_puts("RID: unsupported on this board (needs Wi-Fi/BT CYW43)\n");
}
uint32_t remote_id_active_count(void) { return 0; }

#else // HET68_BT_RID

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"

#define RID_SERVICE_UUID   0xFFFAu
#define RID_APP_CODE       0x0Du
#define RID_MSG_SIZE       25u
#define RID_STALE_MS       15000u
#define RID_LOG_MIN_MS     2000u

static rid_track_t g_tracks[RID_MAX_TRACKS];
static btstack_packet_callback_registration_t g_hci_cb;
static volatile bool g_ready;
static volatile bool g_enabled = true;
static volatile bool g_scan_on;
static uint32_t g_last_log_ms;

static int32_t rd_i32_le(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int16_t rd_i16_le(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static rid_track_t *track_for_addr(const uint8_t addr[6], uint32_t now_ms) {
    int free_i = -1;
    int oldest_i = 0;
    uint32_t oldest_ms = UINT32_MAX;
    for (int i = 0; i < RID_MAX_TRACKS; i++) {
        if (!g_tracks[i].used) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (memcmp(g_tracks[i].addr, addr, 6) == 0)
            return &g_tracks[i];
        if (g_tracks[i].last_ms < oldest_ms) {
            oldest_ms = g_tracks[i].last_ms;
            oldest_i = i;
        }
    }
    int idx = (free_i >= 0) ? free_i : oldest_i;
    memset(&g_tracks[idx], 0, sizeof(g_tracks[idx]));
    g_tracks[idx].used = 1;
    memcpy(g_tracks[idx].addr, addr, 6);
    g_tracks[idx].heading_deg = 361;
    g_tracks[idx].last_ms = now_ms;
    return &g_tracks[idx];
}

static void parse_basic_id(rid_track_t *t, const uint8_t *m) {
    t->id_type = (uint8_t)((m[1] >> 4) & 0x0Fu);
    t->ua_type = (uint8_t)(m[1] & 0x0Fu);
    memcpy(t->uas_id, &m[2], 20);
    t->uas_id[20] = '\0';
    // Trim trailing spaces / NULs already zeroed
    for (int i = 19; i >= 0; i--) {
        if (t->uas_id[i] == ' ' || t->uas_id[i] == '\0')
            t->uas_id[i] = '\0';
        else
            break;
    }
    t->has_basic = 1;
}

static void parse_location(rid_track_t *t, const uint8_t *m) {
    // ASTM F3411 Location/Vector message (type 1), simplified fields.
    t->lat_e7 = rd_i32_le(&m[5]);
    t->lon_e7 = rd_i32_le(&m[9]);
    // SpeedHorizontal: byte3, units 0.25 m/s → cm/s approx
    t->speed_cm_s = (uint16_t)((uint16_t)m[3] * 25u);
    // Direction: byte2, 0..179 means 0..358 deg in 2° steps when Mult flag clear
    if (m[2] <= 179u)
        t->heading_deg = (uint16_t)(m[2] * 2u);
    else
        t->heading_deg = 361u;
    // Geo altitude: bytes 15-16, encoded as (value * 0.5) - 1000 m
    int16_t enc = rd_i16_le(&m[15]);
    t->alt_geoid_m = (int16_t)((enc / 2) - 1000);
    t->has_location = 1;
}

static void parse_odid_message(rid_track_t *t, const uint8_t *m) {
    uint8_t msg_type = (uint8_t)((m[0] >> 4) & 0x0Fu);
    uint8_t ver = (uint8_t)(m[0] & 0x0Fu);
    if (ver > 2u) return;
    switch (msg_type) {
        case 0x0: parse_basic_id(t, m); break;
        case 0x1: parse_location(t, m); break;
        case 0xF: {
            // Message pack: byte1 = count, then count * 25-byte messages
            uint8_t n = m[1];
            if (n > 9u) n = 9u;
            const uint8_t *p = &m[2];
            // Pack body may continue past this 25-byte page in extended adv;
            // for legacy we only have what fits — parse first embedded msg if present.
            if (n >= 1u && (2u + RID_MSG_SIZE) <= RID_MSG_SIZE) {
                // Single-page pack rarely fits a full child; try offset search below.
            }
            (void)p;
            break;
        }
        default:
            break;
    }
}

static bool looks_like_odid_hdr(uint8_t b0) {
    uint8_t t = (uint8_t)((b0 >> 4) & 0x0Fu);
    uint8_t v = (uint8_t)(b0 & 0x0Fu);
    if (v > 2u) return false;
    return (t <= 5u) || (t == 0x0Fu);
}

static void ingest_service_payload(rid_track_t *t, const uint8_t *data, uint8_t len) {
    // Possible layouts after UUID:
    //   [AppCode=0x0D][Counter][25-byte msg…]
    //   [Counter][25-byte msg…]
    //   [25-byte msg…]
    //   message pack spanning service data
    if (len < RID_MSG_SIZE) return;

    uint8_t off = 0;
    if (len >= RID_MSG_SIZE + 2u && data[0] == RID_APP_CODE)
        off = 2;
    else if (len >= RID_MSG_SIZE + 1u && !looks_like_odid_hdr(data[0]) &&
             looks_like_odid_hdr(data[1]))
        off = 1;

    // Walk remaining bytes for 25-byte aligned ODID messages
    while ((uint8_t)(off + RID_MSG_SIZE) <= len) {
        if (looks_like_odid_hdr(data[off])) {
            uint8_t msg_type = (uint8_t)((data[off] >> 4) & 0x0Fu);
            if (msg_type == 0x0Fu && (off + 2u) < len) {
                // Message pack header in this page — parse following full messages
                uint8_t n = data[off + 1u];
                uint8_t p = (uint8_t)(off + 2u);
                for (uint8_t i = 0; i < n && (uint8_t)(p + RID_MSG_SIZE) <= len; i++) {
                    parse_odid_message(t, &data[p]);
                    p = (uint8_t)(p + RID_MSG_SIZE);
                }
                off = p;
                continue;
            }
            parse_odid_message(t, &data[off]);
            off = (uint8_t)(off + RID_MSG_SIZE);
            continue;
        }
        off++;
    }
}

static void maybe_log_track(const rid_track_t *t, uint32_t now_ms) {
    if (!dbg_log_enabled()) return;
    if ((now_ms - g_last_log_ms) < RID_LOG_MIN_MS) return;
    g_last_log_ms = now_ms;

    uint32_t lock = dbg_line_lock();
    dbg_puts("RID id=");
    dbg_puts(t->has_basic ? t->uas_id : "?");
    dbg_puts(" rssi=");
    if (t->rssi < 0) {
        dbg_putc('-');
        dbg_putu32((uint32_t)(-t->rssi));
    } else {
        dbg_putu32((uint32_t)t->rssi);
    }
    if (t->has_location) {
        dbg_puts(" lat=");
        // print lat/lon as integer e7 (compact, no float printf)
        if (t->lat_e7 < 0) { dbg_putc('-'); dbg_putu32((uint32_t)(-t->lat_e7)); }
        else dbg_putu32((uint32_t)t->lat_e7);
        dbg_puts(" lon=");
        if (t->lon_e7 < 0) { dbg_putc('-'); dbg_putu32((uint32_t)(-t->lon_e7)); }
        else dbg_putu32((uint32_t)t->lon_e7);
        dbg_puts(" alt_m=");
        if (t->alt_geoid_m < 0) { dbg_putc('-'); dbg_putu32((uint32_t)(-t->alt_geoid_m)); }
        else dbg_putu32((uint32_t)t->alt_geoid_m);
    }
    dbg_putc('\n');
    dbg_line_unlock(lock);

    // Soft DET event when we have a Basic ID (timestamps need TIME SYNC).
    if (t->has_basic) {
        float conf = 0.5f;
        if (t->rssi > -60) conf = 0.9f;
        else if (t->rssi > -80) conf = 0.7f;
        float inten = (float)t->rssi; // dBm-ish
        (void)detection_log_observe(DET_REMOTEID, 0u, 0.0f, 0.0f, inten, conf);
    }
}

static void handle_adv(const uint8_t *addr, int8_t rssi,
                       const uint8_t *adv, uint8_t adv_len) {
    if (!g_enabled) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    ad_context_t ctx;
    bool matched = false;
    rid_track_t *t = NULL;

    for (ad_iterator_init(&ctx, adv_len, (uint8_t *)adv);
         ad_iterator_has_more(&ctx);
         ad_iterator_next(&ctx)) {
        uint8_t type = ad_iterator_get_data_type(&ctx);
        uint8_t size = ad_iterator_get_data_len(&ctx);
        const uint8_t *data = ad_iterator_get_data(&ctx);
        if (type != BLUETOOTH_DATA_TYPE_SERVICE_DATA) continue;
        if (size < 2u + 1u) continue;
        uint16_t uuid = little_endian_read_16(data, 0);
        if (uuid != RID_SERVICE_UUID) continue;
        if (!t) t = track_for_addr(addr, now);
        t->rssi = rssi;
        t->last_ms = now;
        ingest_service_payload(t, data + 2, (uint8_t)(size - 2u));
        matched = true;
    }

    if (matched && t)
        maybe_log_track(t, now);
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t et = hci_event_packet_get_type(packet);
    if (et == BTSTACK_EVENT_STATE) {
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            g_ready = true;
            if (g_enabled && !g_scan_on) {
                // Passive scanning, ~50% duty (interval/window in 0.625 ms units).
                gap_set_scan_parameters(0 /*passive*/, 96, 48);
                gap_start_scan();
                g_scan_on = true;
                dbg_puts("RID: BLE scan started (OpenDroneID 0xFFFA)\n");
            }
        }
        return;
    }

    if (et == GAP_EVENT_ADVERTISING_REPORT) {
        bd_addr_t address;
        gap_event_advertising_report_get_address(packet, address);
        int8_t rssi = gap_event_advertising_report_get_rssi(packet);
        uint8_t length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);
        handle_adv(address, rssi, data, length);
        return;
    }
}

bool remote_id_init(void) {
    if (cyw43_arch_init()) {
        dbg_puts("RID: cyw43_arch_init FAILED\n");
        return false;
    }

    // BT stack is brought up inside cyw43_arch_init via btstack_cyw43_init.
    l2cap_init();
    sm_init();
    g_hci_cb.callback = &packet_handler;
    hci_add_event_handler(&g_hci_cb);

    gap_set_scan_parameters(0, 96, 48);
    hci_power_control(HCI_POWER_ON);
    g_enabled = true;
    dbg_puts("RID: BT OpenDroneID scanner init OK\n");
    return true;
}

bool remote_id_available(void) { return true; }

void remote_id_set_enabled(bool on) {
    g_enabled = on;
    if (!g_ready) return;
    if (on && !g_scan_on) {
        gap_start_scan();
        g_scan_on = true;
    } else if (!on && g_scan_on) {
        gap_stop_scan();
        g_scan_on = false;
    }
}

bool remote_id_enabled(void) { return g_enabled; }

void remote_id_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < RID_MAX_TRACKS; i++) {
        if (!g_tracks[i].used) continue;
        if ((now - g_tracks[i].last_ms) > RID_STALE_MS)
            g_tracks[i].used = 0;
    }
}

uint32_t remote_id_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < RID_MAX_TRACKS; i++)
        if (g_tracks[i].used) n++;
    return n;
}

uint32_t remote_id_active_count(void) { return remote_id_count(); }

const rid_track_t *remote_id_track(uint32_t index) {
    uint32_t n = 0;
    for (int i = 0; i < RID_MAX_TRACKS; i++) {
        if (!g_tracks[i].used) continue;
        if (n == index) return &g_tracks[i];
        n++;
    }
    return NULL;
}

void remote_id_list_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("RID tracks=");
    dbg_putu32(remote_id_count());
    dbg_puts(" scan=");
    dbg_puts(g_enabled ? "on" : "off");
    dbg_puts(" ready=");
    dbg_puts(g_ready ? "1" : "0");
    dbg_putc('\n');
    for (int i = 0; i < RID_MAX_TRACKS; i++) {
        const rid_track_t *t = &g_tracks[i];
        if (!t->used) continue;
        dbg_puts("  ");
        dbg_puts(t->has_basic ? t->uas_id : "(no-id)");
        dbg_puts(" rssi=");
        if (t->rssi < 0) { dbg_putc('-'); dbg_putu32((uint32_t)(-t->rssi)); }
        else dbg_putu32((uint32_t)t->rssi);
        if (t->has_location) {
            dbg_puts(" lat=");
            if (t->lat_e7 < 0) { dbg_putc('-'); dbg_putu32((uint32_t)(-t->lat_e7)); }
            else dbg_putu32((uint32_t)t->lat_e7);
            dbg_puts(" lon=");
            if (t->lon_e7 < 0) { dbg_putc('-'); dbg_putu32((uint32_t)(-t->lon_e7)); }
            else dbg_putu32((uint32_t)t->lon_e7);
        }
        dbg_puts(" mac=");
        for (int b = 0; b < 6; b++) {
            dbg_puthex8(t->addr[b]);
            if (b < 5) dbg_putc(':');
        }
        dbg_putc('\n');
    }
    dbg_line_unlock(lock);
}

#endif // HET68_BT_RID
