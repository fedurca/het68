// acoustic_link.h — node-to-node acoustic link over the GP6/GP7 4 kHz piezo.
//
// PHY : DSSS-BPSK on the existing PS1240 H-bridge beacon engine (buzzer.c).
// MAC : per-node CDMA code (Gold family) + slotted-ALOHA jitter, fixed frame.
// SEC : Hamming(7,4) FEC + CRC32, reserved key_id/nonce/flags for future AEAD.
// APP : BEACON (ranging + coarse time), DETECT (summary), CTRL (wifi-wake/OTA).
//
// RX rides on the 6-mic 48 kHz capture: main.c feeds a mono mix to
// acoustic_link_rx_push(); acoustic_link_poll() runs a bounded matched filter.
// No malloc, no blocking in the realtime path (see AGENTS.md).
#pragma once
#include <stdint.h>
#include <stdbool.h>

// --- PHY timing (locked to the buzzer carrier) ------------------------------
#define ALINK_CARRIER_HZ     4000u
#define ALINK_FS_HZ          48000u
#define ALINK_SAMPLES_CYCLE  (ALINK_FS_HZ / ALINK_CARRIER_HZ)          // 12
#define ALINK_CYCLES_CHIP    8u
#define ALINK_SAMPLES_CHIP   (ALINK_SAMPLES_CYCLE * ALINK_CYCLES_CHIP) // 96
#define ALINK_CHIP_HZ        (ALINK_FS_HZ / ALINK_SAMPLES_CHIP)        // 500

// --- Spreading / framing ----------------------------------------------------
#define ALINK_PREAMBLE_LEN   127u   // length-127 m-sequence / Gold code
#define ALINK_DATA_SF        8u     // chips per coded data bit
#define ALINK_MAX_NODES      8u     // distinct CDMA codes tracked on RX

#define ALINK_VERSION        1u
#define ALINK_MAX_PAYLOAD    16u
// Fixed on-air plaintext frame (padded): keeps RX sizing constant.
//   [0]=ver [1]=node_id [2]=type [3]=seq [4]=flags [5]=key_id
//   [6..9]=nonce [10]=len [11..26]=payload[16] [27..30]=crc32
#define ALINK_FRAME_BYTES    31u
// Hamming(7,4): 2 nibbles/byte * 7 bits.
#define ALINK_CODED_BITS     (ALINK_FRAME_BYTES * 2u * 7u)             // 434
#define ALINK_TX_CHIPS       (ALINK_PREAMBLE_LEN + ALINK_CODED_BITS * ALINK_DATA_SF) // 3599

// Frame types.
enum {
    ALINK_FT_BEACON = 0,   // ranging + coarse time sync
    ALINK_FT_DETECT = 1,   // compact detection summary
    ALINK_FT_CTRL   = 2,   // control-plane (wifi wake, OTA request)
    ALINK_FT_ACK    = 3,
};

// CTRL subtypes (payload[0]).
enum {
    ALINK_CTRL_WIFI_WAKE = 1,
    ALINK_CTRL_OTA_REQ   = 2,
};

// Header flags.
#define ALINK_FLAG_ENCRYPTED  0x01u   // payload sealed (reserved; stub today)
#define ALINK_FLAG_ACK_REQ    0x02u
#define ALINK_FLAG_SYNCED     0x04u   // sender clock is synced (epoch valid)

// Decoded frame handed to the dispatcher / CLI.
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
    uint64_t rx_time_us;   // local monotonic ToA (matched-filter peak)
    float    corr_q;       // normalized correlation quality 0..1
} alink_frame_t;

void acoustic_link_init(uint8_t node_id);
void acoustic_link_set_node_id(uint8_t id);
uint8_t acoustic_link_node_id(void);

// Queue one frame for transmission (non-blocking). Payload padded to 16 B.
// Returns false if the PHY is busy or args invalid.
bool acoustic_link_send(uint8_t type, uint8_t flags,
                        const uint8_t *payload, uint8_t len);

// Convenience: broadcast a CTRL WIFI_WAKE with a rendezvous token.
bool acoustic_link_send_wifi_wake(uint8_t channel_token);

// RX sample sink — cheap, called from the USB/I2S frame builder (core0).
void acoustic_link_rx_push(int16_t mono);

// Main-loop service: drains the RX ring (bounded), runs the matched filter,
// dispatches frames, and schedules periodic beacons. Call each loop iteration.
void acoustic_link_poll(void);

// Diagnostics.
uint32_t acoustic_link_tx_count(void);
uint32_t acoustic_link_rx_count(void);
uint32_t acoustic_link_rx_bad_crc(void);
bool     acoustic_link_tx_busy(void);
bool     acoustic_link_wifi_wake_pending(void);
void     acoustic_link_status_uart(void);
