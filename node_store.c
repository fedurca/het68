// node_store.c — peer table + acoustic single-sided two-way ranging (SS-TWR).
//
// SS-TWR with broadcast beacons:
//   A sends poll seqA at t1(A).  B hears it at t2(B).
//   B sends beacon at t3(B) echoing {A, seqA, t_reply=t3-t2}.
//   A hears B at t4(A); Tround = t4 - t1; ToF = (Tround - t_reply)/2.
//   distance = ToF * c(baro).   offset(A-B) = t4 - t3 - ToF.
// RAM-only, bounded, no blocking.
#include "node_store.h"
#include "debug_io.h"
#include <string.h>

// Ring of our recent outgoing poll timestamps, looked up by seq on echo.
#define POLL_HIST 8u
static uint8_t  g_poll_seq[POLL_HIST];
static uint64_t g_poll_us[POLL_HIST];
static uint32_t g_poll_head;
static bool     g_poll_valid[POLL_HIST];

static node_slot_t g_slots[NODE_STORE_MAX];
static uint32_t    g_count;
static int         g_last_heard = -1;   // slot index of most recent beacon

// Sanity window for a valid time-of-flight (0..~0.6 s ~= 200 m).
#define TOF_MAX_US 600000

static void put_f2(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 100.0f + 0.5f);
    if (fp >= 100u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    if (fp < 10u) dbg_putc('0');
    dbg_putu32(fp);
}

static void put_i64(int64_t v) {
    if (v < 0) { dbg_putc('-'); v = -v; }
    dbg_putu32((uint32_t)v);
}

void node_store_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_poll_valid, 0, sizeof(g_poll_valid));
    g_count = 0;
    g_poll_head = 0;
    g_last_heard = -1;
}

void node_store_note_tx(uint8_t seq, uint64_t tx_time_us) {
    g_poll_seq[g_poll_head] = seq;
    g_poll_us[g_poll_head] = tx_time_us;
    g_poll_valid[g_poll_head] = true;
    g_poll_head = (g_poll_head + 1u) % POLL_HIST;
}

static bool lookup_poll(uint8_t seq, uint64_t *t1_out) {
    for (uint32_t i = 0; i < POLL_HIST; i++) {
        if (g_poll_valid[i] && g_poll_seq[i] == seq) {
            *t1_out = g_poll_us[i];
            return true;
        }
    }
    return false;
}

static int find_or_alloc(uint8_t node_id) {
    int free_i = -1, oldest_i = 0;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < NODE_STORE_MAX; i++) {
        if (g_slots[i].used) {
            if (g_slots[i].node_id == node_id) return (int)i;
            if (g_slots[i].last_rx_us < oldest) {
                oldest = g_slots[i].last_rx_us;
                oldest_i = (int)i;
            }
        } else if (free_i < 0) {
            free_i = (int)i;
        }
    }
    int idx = (free_i >= 0) ? free_i : oldest_i;
    memset(&g_slots[idx], 0, sizeof(g_slots[idx]));
    g_slots[idx].used = 1;
    g_slots[idx].node_id = node_id;
    return idx;
}

void node_store_on_beacon(uint8_t self_id, uint8_t peer_id, uint8_t peer_seq,
                          uint64_t rx_time_us,
                          uint32_t peer_tx_mono_us,
                          uint8_t echo_node, uint8_t echo_seq,
                          uint32_t t_reply_us,
                          bool peer_synced,
                          float corr_q, float c_sound_m_s) {
    int idx = find_or_alloc(peer_id);
    node_slot_t *s = &g_slots[idx];
    s->corr_q = corr_q;
    s->synced = peer_synced ? 1u : 0u;
    s->last_rx_us = rx_time_us;
    s->last_peer_seq = peer_seq;
    s->rx_count++;
    g_last_heard = idx;

    // Recount live peers.
    uint32_t n = 0;
    for (uint32_t i = 0; i < NODE_STORE_MAX; i++) if (g_slots[i].used) n++;
    g_count = n;

    // Ranging: this beacon echoes one of our polls.
    if (echo_node == self_id) {
        uint64_t t1 = 0;
        if (lookup_poll(echo_seq, &t1) && rx_time_us > t1) {
            uint64_t tround = rx_time_us - t1;
            if ((uint64_t)t_reply_us < tround) {
                int64_t tof = ((int64_t)tround - (int64_t)t_reply_us) / 2;
                if (tof >= 0 && tof < TOF_MAX_US) {
                    float dist = (float)tof * 1e-6f * c_sound_m_s;
                    s->distance_m = dist;
                    s->rng_valid = 1u;
                    // Coarse clock offset: t4 - t3 - ToF, with t3 = peer_tx_mono.
                    s->clock_offset_us =
                        (int64_t)rx_time_us - (int64_t)peer_tx_mono_us - tof;
                }
            }
        }
    }
}

int node_store_pending_echo(uint8_t *echo_node, uint8_t *echo_seq,
                            uint32_t *t_reply_us, uint64_t now_us) {
    if (g_last_heard < 0) return -1;
    node_slot_t *s = &g_slots[g_last_heard];
    if (!s->used) return -1;
    if (now_us <= s->last_rx_us) return -1;
    uint64_t reply = now_us - s->last_rx_us;
    if (reply > 0xFFFFFFFFull) return -1;   // too stale to echo
    *echo_node = s->node_id;
    *echo_seq = s->last_peer_seq;
    *t_reply_us = (uint32_t)reply;
    return g_last_heard;
}

uint32_t node_store_count(void) { return g_count; }

const node_slot_t *node_store_slot(uint32_t index) {
    if (index >= NODE_STORE_MAX) return NULL;
    if (!g_slots[index].used) return NULL;
    return &g_slots[index];
}

void node_store_stats_uart(void) {
    dbg_puts("NODE peers=");
    dbg_putu32(g_count);
    dbg_putc('\n');
}

void node_store_list_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("=== node peers ===\n");
    node_store_stats_uart();
    if (g_count == 0) dbg_puts("(none)\n");
    for (uint32_t i = 0; i < NODE_STORE_MAX; i++) {
        const node_slot_t *s = &g_slots[i];
        if (!s->used) continue;
        dbg_puts("NODE id=");
        dbg_putu32(s->node_id);
        dbg_puts(" rx=");
        dbg_putu32(s->rx_count);
        dbg_puts(" q=");
        put_f2(s->corr_q);
        if (s->rng_valid) {
            dbg_puts(" dist_m=");
            put_f2(s->distance_m);
            dbg_puts(" offset_us=");
            put_i64(s->clock_offset_us);
        } else {
            dbg_puts(" dist=?");
        }
        dbg_puts(" synced=");
        dbg_puts(s->synced ? "1" : "0");
        dbg_putc('\n');
    }
    dbg_puts("=== end node peers ===\n");
    dbg_line_unlock(lock);
}
