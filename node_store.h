// node_store.h — peer table + acoustic two-way ranging / clock offset.
//
// Fed by acoustic_link on every decoded BEACON. Holds, per neighbour node:
// distance (single-sided two-way ranging), clock offset, correlation quality,
// and last-seen time. RAM-only (bounded); no flash dependency.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NODE_STORE_MAX 8u

typedef struct {
    uint8_t  used;
    uint8_t  node_id;
    uint8_t  rng_valid;      // distance_m is meaningful
    uint8_t  synced;         // peer advertised a synced clock
    float    distance_m;     // SS-TWR estimate
    float    corr_q;         // last correlation quality 0..1
    int64_t  clock_offset_us;// our_clock - peer_clock (approx)
    uint32_t rx_count;       // beacons heard from this peer
    uint64_t last_rx_us;     // local time of last beacon
    uint8_t  last_peer_seq;  // sender's last frame seq (for echoing back)
} node_slot_t;

void node_store_init(void);

// Record that we transmitted a beacon (poll) with this seq at this local time.
void node_store_note_tx(uint8_t seq, uint64_t tx_time_us);

// Process a decoded BEACON from peer_id received locally at rx_time_us.
//   peer_seq        : sender's own frame seq (so we can echo it back)
//   peer_tx_mono_us : sender monotonic microseconds (low 32, from payload)
//   echo_node       : peer's echoed target node id (0xFF = none)
//   echo_seq        : echoed seq (matches our poll seq when echo_node == us)
//   t_reply_us      : responder turnaround (t3 - t2) for SS-TWR
//   corr_q          : matched-filter quality
//   c_sound_m_s     : current speed of sound (baro-corrected)
void node_store_on_beacon(uint8_t self_id, uint8_t peer_id, uint8_t peer_seq,
                          uint64_t rx_time_us,
                          uint32_t peer_tx_mono_us,
                          uint8_t echo_node, uint8_t echo_seq,
                          uint32_t t_reply_us,
                          bool peer_synced,
                          float corr_q, float c_sound_m_s);

// If we heard peer_id, return its slot index and the turnaround we owe it
// (rx->tx delta) so our next beacon can echo it. Returns -1 if none pending.
int node_store_pending_echo(uint8_t *echo_node, uint8_t *echo_seq,
                            uint32_t *t_reply_us, uint64_t now_us);

uint32_t node_store_count(void);
const node_slot_t *node_store_slot(uint32_t index);
void node_store_list_uart(void);
void node_store_stats_uart(void);
