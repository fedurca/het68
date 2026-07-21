// acoustic_link.h — node-to-node acoustic link over the GP6/GP7 PS1240 piezo.
//
// PHY (v1.4 / wire version 2):
//   Fast FHSS on the PS1240. Fixed 250 µs chips, 31-chip m-sequence preamble
//   (BPSK @ 4 kHz) for CDMA acquisition, then data as non-coherent 2-FSK on an
//   8-tone hop set around resonance — one frame ~70 ms (< 100 ms) and far less
//   tonal than the old ~7 s DSSS whistle.
// MAC : per-node CDMA preamble code + slotted-ALOHA jitter, fixed frame.
// SEC : CRC32 (Hamming dropped for air-time); reserved key_id/nonce/flags for AEAD.
// APP : BEACON (ranging + coarse time), DETECT, CTRL (wifi-wake/OTA).
//
// RX rides on the 6-mic 48 kHz capture. No malloc, no blocking in the realtime
// path (see AGENTS.md).
#pragma once
#include <stdint.h>
#include <stdbool.h>

// --- PHY timing -------------------------------------------------------------
#define ALINK_FS_HZ          48000u
#define ALINK_CHIP_US        250u
#define ALINK_SAMPLES_CHIP   ((ALINK_FS_HZ * ALINK_CHIP_US) / 1000000u)  // 12
#define ALINK_PREAMBLE_HZ    4000u    // fixed-frequency acquisition carrier

// --- Spreading / framing ----------------------------------------------------
#define ALINK_PREAMBLE_LEN   31u    // degree-5 Gold family
#define ALINK_DATA_SF        1u     // one chip per data bit (air-time critical)
#define ALINK_MAX_NODES      8u
#define ALINK_NHOPS          8u     // FHSS hop-set size (data chips)

#define ALINK_VERSION        2u     // wire version (1.3.x used version 1 + Hamming)
#define ALINK_MAX_PAYLOAD    16u
// Fixed on-air plaintext frame (padded):
//   [0]=ver [1]=node_id [2]=type [3]=seq [4]=flags [5]=key_id
//   [6..9]=nonce [10]=len [11..26]=payload[16] [27..30]=crc32
#define ALINK_FRAME_BYTES    31u
#define ALINK_RAW_BITS       (ALINK_FRAME_BYTES * 8u)                    // 248
#define ALINK_TX_CHIPS       (ALINK_PREAMBLE_LEN + ALINK_RAW_BITS * ALINK_DATA_SF) // 279
// Air-time: 279 * 250 µs = 69.75 ms (< 100 ms).

// Frame types.
enum {
    ALINK_FT_BEACON = 0,
    ALINK_FT_DETECT = 1,
    ALINK_FT_CTRL   = 2,
    ALINK_FT_ACK    = 3,
};

enum {
    ALINK_CTRL_WIFI_WAKE = 1,
    ALINK_CTRL_OTA_REQ   = 2,
};

#define ALINK_FLAG_ENCRYPTED  0x01u
#define ALINK_FLAG_ACK_REQ    0x02u
#define ALINK_FLAG_SYNCED     0x04u

typedef struct {
    uint8_t  version;
    uint8_t  node_id;
    uint8_t  type;
    uint8_t  seq;
    uint8_t  flags;
    uint8_t  key_id;
    uint32_t nonce;
    uint8_t  len;
    uint8_t  payload[ALINK_MAX_PAYLOAD];
    uint64_t rx_time_us;
    float    corr_q;
} alink_frame_t;

void acoustic_link_init(uint8_t node_id);
void acoustic_link_set_node_id(uint8_t id);
uint8_t acoustic_link_node_id(void);

bool acoustic_link_send(uint8_t type, uint8_t flags,
                        const uint8_t *payload, uint8_t len);
bool acoustic_link_send_wifi_wake(uint8_t channel_token);

void acoustic_link_rx_push(int16_t mono);
void acoustic_link_poll(void);

uint32_t acoustic_link_tx_count(void);
uint32_t acoustic_link_rx_count(void);
uint32_t acoustic_link_rx_bad_crc(void);
bool     acoustic_link_tx_busy(void);
bool     acoustic_link_wifi_wake_pending(void);
void     acoustic_link_status_uart(void);

// Nominal air-time of one full chirp frame in microseconds.
uint32_t acoustic_link_frame_air_us(void);
