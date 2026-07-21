// acoustic_link.c — fast FHSS-BPSK node link on the GP6/GP7 PS1240 + mic RX.
// Wire version 2 (firmware 1.4.0): ~70 ms/frame. See acoustic_link.h.
#include "acoustic_link.h"
#include "buzzer.h"
#include "node_store.h"
#include "het68_time.h"
#include "doa.h"
#include "debug_io.h"
#include "pico/stdlib.h"
#include <math.h>
#include <string.h>

#ifndef HET68_WIFI_WAKE
#define HET68_WIFI_WAKE 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Node identity + degree-5 Gold preamble codes (length 31)
// ---------------------------------------------------------------------------
static uint8_t g_node_id;

static int8_t g_pre_code[ALINK_MAX_NODES][ALINK_PREAMBLE_LEN];   // +1/-1

// Hop set around PS1240 resonance (~4 kHz). Nearby tones still couple into the
// element; hopping breaks the long tonal whistle that made v1.3 unbearable.
static const uint16_t g_hop_hz[ALINK_NHOPS] = {
    3000u, 3400u, 3800u, 4000u, 4200u, 4600u, 5000u, 5400u
};

static void build_mseq5(uint8_t tap_a, uint8_t tap_b, uint8_t out[ALINK_PREAMBLE_LEN]) {
    uint8_t lfsr = 0x1Fu;
    for (uint32_t i = 0; i < ALINK_PREAMBLE_LEN; i++) {
        uint8_t bit = (uint8_t)(((lfsr >> tap_a) ^ (lfsr >> tap_b)) & 1u);
        out[i] = (uint8_t)(lfsr & 1u);
        lfsr = (uint8_t)(((lfsr << 1) | bit) & 0x1Fu);
    }
}

static void build_codes(void) {
    // Length-31 m-sequence with distinct circular shifts per node (CDMA).
    uint8_t a[ALINK_PREAMBLE_LEN];
    build_mseq5(4u, 1u, a);   // x^5 + x^2 + 1
    for (uint32_t n = 0; n < ALINK_MAX_NODES; n++) {
        uint32_t shift = n * 3u;   // well-separated phases in 31-space
        for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++) {
            uint8_t g = a[(k + shift) % ALINK_PREAMBLE_LEN];
            g_pre_code[n][k] = g ? -1 : +1;
        }
    }
}

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3)
// ---------------------------------------------------------------------------
static uint32_t crc32(const uint8_t *d, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return ~c;
}

// ---------------------------------------------------------------------------
// Crypto hook (reserved stub)
// ---------------------------------------------------------------------------
static bool crypto_seal(uint8_t *payload, uint8_t len, uint8_t key_id, uint32_t nonce) {
    (void)payload; (void)len; (void)key_id; (void)nonce;
    return true;
}
static bool crypto_open(uint8_t *payload, uint8_t len, uint8_t key_id, uint32_t nonce) {
    (void)payload; (void)len; (void)key_id; (void)nonce;
    return true;
}

// ---------------------------------------------------------------------------
// TX
// ---------------------------------------------------------------------------
static uint8_t  s_tx_phase[ALINK_TX_CHIPS];
static uint16_t s_tx_freq[ALINK_TX_CHIPS];
static uint8_t  s_tx_seq;
static uint32_t s_tx_count;
static uint8_t  s_pending_seq;
static bool     s_pending_note;
static uint64_t s_last_tx_start_us;

static void build_tx_chips(const uint8_t frame[ALINK_FRAME_BYTES]) {
    uint32_t ci = 0;
    const int8_t *pre = g_pre_code[g_node_id % ALINK_MAX_NODES];

    // Preamble: fixed 4 kHz, Gold phase code (CDMA acquisition).
    for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++) {
        s_tx_phase[ci] = (pre[k] < 0) ? 1u : 0u;
        s_tx_freq[ci] = ALINK_PREAMBLE_HZ;
        ci++;
    }

    // Data: raw frame bits as non-coherent 2-FSK on the hop set.
    // PWM retune destroys absolute carrier phase, so BPSK-on-hop is unreliable;
    // each bit picks one of two hop tones (energy detection on RX).
    for (uint32_t i = 0; i < ALINK_FRAME_BYTES; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t bit = (uint8_t)((frame[i] >> b) & 1u);
            uint32_t di = ci - ALINK_PREAMBLE_LEN;
            uint32_t base = (di + (uint32_t)g_node_id * 3u) % ALINK_NHOPS;
            uint32_t hi = (base + (bit ? (ALINK_NHOPS / 2u) : 0u)) % ALINK_NHOPS;
            s_tx_phase[ci] = 0u;   // fixed drive polarity; info is in frequency
            s_tx_freq[ci] = g_hop_hz[hi];
            ci++;
        }
    }
}

bool acoustic_link_send(uint8_t type, uint8_t flags,
                        const uint8_t *payload, uint8_t len) {
    if (len > ALINK_MAX_PAYLOAD) return false;
    if (buzzer_tx_busy()) return false;

    uint8_t frame[ALINK_FRAME_BYTES];
    memset(frame, 0, sizeof(frame));
    uint8_t key_id = 0;
    uint32_t nonce = 0;
    frame[0] = ALINK_VERSION;
    frame[1] = g_node_id;
    frame[2] = type;
    frame[3] = s_tx_seq;
    frame[4] = flags;
    frame[5] = key_id;
    frame[6] = (uint8_t)(nonce & 0xFF);
    frame[7] = (uint8_t)((nonce >> 8) & 0xFF);
    frame[8] = (uint8_t)((nonce >> 16) & 0xFF);
    frame[9] = (uint8_t)((nonce >> 24) & 0xFF);
    frame[10] = len;
    if (payload && len) memcpy(&frame[11], payload, len);

    if (flags & ALINK_FLAG_ENCRYPTED)
        (void)crypto_seal(&frame[11], ALINK_MAX_PAYLOAD, key_id, nonce);

    uint32_t crc = crc32(frame, 27);
    frame[27] = (uint8_t)(crc & 0xFF);
    frame[28] = (uint8_t)((crc >> 8) & 0xFF);
    frame[29] = (uint8_t)((crc >> 16) & 0xFF);
    frame[30] = (uint8_t)((crc >> 24) & 0xFF);

    build_tx_chips(frame);
    if (!buzzer_tx_chips_fh(s_tx_phase, s_tx_freq, ALINK_TX_CHIPS)) return false;

    s_pending_seq = s_tx_seq;
    s_pending_note = (type == ALINK_FT_BEACON);
    s_tx_seq++;
    s_tx_count++;
    return true;
}

// ---------------------------------------------------------------------------
// Beacon scheduling
// ---------------------------------------------------------------------------
#define BEACON_BASE_MS   2000u
#define BEACON_JITTER_MS 800u
static uint64_t s_next_beacon_us;
static uint32_t s_lcg = 0x1234567u;

static uint32_t rnd(void) { s_lcg = s_lcg * 1664525u + 1013904223u; return s_lcg; }

static void schedule_tx(void) {
    uint64_t now = time_us_64();
    if (s_next_beacon_us == 0) {
        s_next_beacon_us = now + (uint64_t)BEACON_BASE_MS * 1000u;
        return;
    }
    if (now < s_next_beacon_us) return;
    if (buzzer_tx_busy()) return;

    uint8_t p[ALINK_MAX_PAYLOAD];
    memset(p, 0, sizeof(p));
    uint32_t tx_mono = (uint32_t)now;
    uint32_t epoch = het68_time_synced() ? het68_time_epoch_sec() : 0u;
    p[0] = (uint8_t)(tx_mono & 0xFF);
    p[1] = (uint8_t)((tx_mono >> 8) & 0xFF);
    p[2] = (uint8_t)((tx_mono >> 16) & 0xFF);
    p[3] = (uint8_t)((tx_mono >> 24) & 0xFF);
    p[4] = (uint8_t)(epoch & 0xFF);
    p[5] = (uint8_t)((epoch >> 8) & 0xFF);
    p[6] = (uint8_t)((epoch >> 16) & 0xFF);
    p[7] = (uint8_t)((epoch >> 24) & 0xFF);

    uint8_t echo_node = 0xFF, echo_seq = 0;
    uint32_t t_reply = 0;
    (void)node_store_pending_echo(&echo_node, &echo_seq, &t_reply, now);
    p[8] = echo_node;
    p[9]  = (uint8_t)(t_reply & 0xFF);
    p[10] = (uint8_t)((t_reply >> 8) & 0xFF);
    p[11] = (uint8_t)((t_reply >> 16) & 0xFF);
    p[12] = (uint8_t)((t_reply >> 24) & 0xFF);
    p[13] = echo_seq;
    p[14] = het68_time_synced() ? 1u : 0u;

    uint8_t flags = het68_time_synced() ? ALINK_FLAG_SYNCED : 0u;
    (void)acoustic_link_send(ALINK_FT_BEACON, flags, p, ALINK_MAX_PAYLOAD);

    uint32_t jitter = rnd() % (BEACON_JITTER_MS * 1000u);
    s_next_beacon_us = now + (uint64_t)BEACON_BASE_MS * 1000u + jitter;
}

// ---------------------------------------------------------------------------
// RX ring
// ---------------------------------------------------------------------------
#define RX_RING 8192u
static int16_t          s_rx_ring[RX_RING];
static volatile uint32_t s_rx_head;
static volatile uint32_t s_rx_tail;

void acoustic_link_rx_push(int16_t mono) {
    uint32_t h = s_rx_head;
    uint32_t nh = (h + 1u) & (RX_RING - 1u);
    if (nh == s_rx_tail) return;
    s_rx_ring[h] = mono;
    s_rx_head = nh;
}

// ---------------------------------------------------------------------------
// RX: 4 kHz preamble correlator + FHSS data demod
// ---------------------------------------------------------------------------
static float s_cos4[ALINK_SAMPLES_CHIP];
static float s_sin4[ALINK_SAMPLES_CHIP];
// Per-hop baseband LUTs over one chip window (12 samples).
static float s_hop_cos[ALINK_NHOPS][ALINK_SAMPLES_CHIP];
static float s_hop_sin[ALINK_NHOPS][ALINK_SAMPLES_CHIP];

static float s_chip_buf[ALINK_SAMPLES_CHIP];
static uint32_t s_chip_fill;

static float s_hist_i[ALINK_PREAMBLE_LEN];
static float s_hist_q[ALINK_PREAMBLE_LEN];
static uint32_t s_hist_head;
static uint32_t s_hist_fill;

enum { RX_SEARCH = 0, RX_DATA };
static uint8_t  s_rx_state;
static uint8_t  s_rx_node;
static float    s_rx_cos, s_rx_sin;
static uint64_t s_rx_toa_us;
static float    s_rx_q;
static uint32_t s_bit_idx;
static uint8_t  s_raw_bits[ALINK_RAW_BITS];

static uint32_t s_rx_count;
static uint32_t s_rx_bad_crc;
static volatile bool s_wifi_wake_pending;

#define DETECT_RHO   0.28f
#define DETECT_EMIN  5.0e5f

static void rx_reset_search(void) {
    s_rx_state = RX_SEARCH;
    s_bit_idx = 0;
}

static void dispatch_frame(const alink_frame_t *f);

static void finalize_frame(void) {
    uint8_t bytes[ALINK_FRAME_BYTES];
    for (uint32_t i = 0; i < ALINK_FRAME_BYTES; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++)
            v = (uint8_t)((v << 1) | (s_raw_bits[i * 8u + (uint32_t)b] & 1u));
        bytes[i] = v;
    }
    uint32_t crc = crc32(bytes, 27);
    uint32_t rx_crc = (uint32_t)bytes[27] | ((uint32_t)bytes[28] << 8) |
                      ((uint32_t)bytes[29] << 16) | ((uint32_t)bytes[30] << 24);
    if (crc != rx_crc || bytes[0] != ALINK_VERSION) {
        s_rx_bad_crc++;
        rx_reset_search();
        return;
    }
    alink_frame_t f;
    f.version = bytes[0];
    f.node_id = bytes[1];
    f.type = bytes[2];
    f.seq = bytes[3];
    f.flags = bytes[4];
    f.key_id = bytes[5];
    f.nonce = (uint32_t)bytes[6] | ((uint32_t)bytes[7] << 8) |
              ((uint32_t)bytes[8] << 16) | ((uint32_t)bytes[9] << 24);
    f.len = bytes[10] > ALINK_MAX_PAYLOAD ? ALINK_MAX_PAYLOAD : bytes[10];
    memcpy(f.payload, &bytes[11], ALINK_MAX_PAYLOAD);
    f.rx_time_us = s_rx_toa_us;
    f.corr_q = s_rx_q;
    if (f.flags & ALINK_FLAG_ENCRYPTED)
        (void)crypto_open(f.payload, ALINK_MAX_PAYLOAD, f.key_id, f.nonce);
    // Prefer header node_id when it matches the CDMA detection.
    if (f.node_id >= ALINK_MAX_NODES) f.node_id = s_rx_node;
    s_rx_count++;
    dispatch_frame(&f);
    rx_reset_search();
}

static void mix_chip(const float *buf, const float *c, const float *s,
                     float *oi, float *oq) {
    float ri = 0.0f, rq = 0.0f;
    for (uint32_t n = 0; n < ALINK_SAMPLES_CHIP; n++) {
        ri += buf[n] * c[n];
        rq += buf[n] * s[n];
    }
    *oi = ri;
    *oq = rq;
}

static void rx_on_chip(const float *buf, uint64_t chip_us) {
    if (s_rx_state == RX_DATA) {
        uint32_t di = s_bit_idx;
        uint32_t base = (di + (uint32_t)s_rx_node * 3u) % ALINK_NHOPS;
        uint32_t h0 = base;
        uint32_t h1 = (base + ALINK_NHOPS / 2u) % ALINK_NHOPS;
        float i0, q0, i1, q1;
        mix_chip(buf, s_hop_cos[h0], s_hop_sin[h0], &i0, &q0);
        mix_chip(buf, s_hop_cos[h1], s_hop_sin[h1], &i1, &q1);
        float e0 = i0 * i0 + q0 * q0;
        float e1 = i1 * i1 + q1 * q1;
        s_raw_bits[s_bit_idx] = (e1 > e0) ? 1u : 0u;
        s_bit_idx++;
        if (s_bit_idx >= ALINK_RAW_BITS) finalize_frame();
        return;
    }

    // SEARCH on fixed 4 kHz preamble.
    float ci, cq;
    mix_chip(buf, s_cos4, s_sin4, &ci, &cq);
    s_hist_i[s_hist_head] = ci;
    s_hist_q[s_hist_head] = cq;
    s_hist_head = (s_hist_head + 1u) % ALINK_PREAMBLE_LEN;
    if (s_hist_fill < ALINK_PREAMBLE_LEN) { s_hist_fill++; return; }

    float energy = 0.0f;
    for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++)
        energy += s_hist_i[k] * s_hist_i[k] + s_hist_q[k] * s_hist_q[k];
    if (energy < DETECT_EMIN) return;

    int best_node = -1;
    float best_mag2 = 0.0f, best_ci = 0.0f, best_cq = 0.0f;
    for (uint32_t n = 0; n < ALINK_MAX_NODES; n++) {
        const int8_t *code = g_pre_code[n];
        float ri = 0.0f, rq = 0.0f;
        uint32_t idx = s_hist_head;
        for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++) {
            float c = (float)code[k];
            ri += c * s_hist_i[idx];
            rq += c * s_hist_q[idx];
            idx = (idx + 1u) % ALINK_PREAMBLE_LEN;
        }
        float mag2 = ri * ri + rq * rq;
        if (mag2 > best_mag2) {
            best_mag2 = mag2;
            best_node = (int)n;
            best_ci = ri;
            best_cq = rq;
        }
    }

    float rho = best_mag2 / ((float)ALINK_PREAMBLE_LEN * energy);
    if (best_node >= 0 && rho > DETECT_RHO) {
        float mag = sqrtf(best_mag2);
        if (mag < 1.0f) return;
        s_rx_node = (uint8_t)best_node;
        s_rx_cos = best_ci / mag;
        s_rx_sin = best_cq / mag;
        s_rx_toa_us = chip_us;
        s_rx_q = rho;
        s_rx_state = RX_DATA;
        s_bit_idx = 0;
        s_hist_fill = 0;
    }
}

void acoustic_link_poll(void) {
    schedule_tx();

    if (s_pending_note) {
        uint64_t st = buzzer_tx_start_us();
        if (st != 0 && st != s_last_tx_start_us) {
            node_store_note_tx(s_pending_seq, st);
            s_last_tx_start_us = st;
            s_pending_note = false;
        }
    }

    uint32_t head = s_rx_head;
    uint32_t tail = s_rx_tail;
    if (head == tail) return;

    uint64_t now = time_us_64();
    uint32_t avail = (head - tail) & (RX_RING - 1u);
    uint32_t remaining = avail;
    while (tail != head) {
        int16_t x = s_rx_ring[tail];
        tail = (tail + 1u) & (RX_RING - 1u);
        remaining--;
        s_chip_buf[s_chip_fill++] = (float)x;
        if (s_chip_fill >= ALINK_SAMPLES_CHIP) {
            uint64_t chip_us = now - (uint64_t)remaining * 1000000ull / ALINK_FS_HZ;
            rx_on_chip(s_chip_buf, chip_us);
            s_chip_fill = 0;
        }
    }
    s_rx_tail = tail;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
#if HET68_WIFI_WAKE
static void wifi_wake(uint8_t token) {
    s_wifi_wake_pending = true;
    uint32_t lock = dbg_line_lock();
    dbg_puts("LINK: WIFI_WAKE token=");
    dbg_putu32(token);
    dbg_puts(" — Wi-Fi STA bring-up requested (bulk sync / OTA)\n");
    dbg_line_unlock(lock);
}
#else
static void wifi_wake(uint8_t token) {
    (void)token;
    s_wifi_wake_pending = true;
    dbg_puts("LINK: WIFI_WAKE received — no Wi-Fi on this board\n");
}
#endif

static void dispatch_frame(const alink_frame_t *f) {
    switch (f->type) {
        case ALINK_FT_BEACON: {
            uint32_t peer_tx = (uint32_t)f->payload[0] | ((uint32_t)f->payload[1] << 8) |
                               ((uint32_t)f->payload[2] << 16) | ((uint32_t)f->payload[3] << 24);
            uint32_t epoch   = (uint32_t)f->payload[4] | ((uint32_t)f->payload[5] << 8) |
                               ((uint32_t)f->payload[6] << 16) | ((uint32_t)f->payload[7] << 24);
            uint8_t echo_node = f->payload[8];
            uint32_t t_reply = (uint32_t)f->payload[9] | ((uint32_t)f->payload[10] << 8) |
                               ((uint32_t)f->payload[11] << 16) | ((uint32_t)f->payload[12] << 24);
            uint8_t echo_seq = f->payload[13];
            bool peer_synced = (f->flags & ALINK_FLAG_SYNCED) != 0;

            node_store_on_beacon(g_node_id, f->node_id, f->seq, f->rx_time_us,
                                 peer_tx, echo_node, echo_seq, t_reply,
                                 peer_synced, f->corr_q, doa_c_sound_m_s());

            if (peer_synced && epoch > 1700000000u && !het68_time_synced())
                (void)het68_time_sync_from(epoch, HET68_TIME_SRC_ACOUSTIC, 60u);
            break;
        }
        case ALINK_FT_CTRL: {
            uint8_t sub = f->payload[0];
            if (sub == ALINK_CTRL_WIFI_WAKE) wifi_wake(f->payload[1]);
            else if (sub == ALINK_CTRL_OTA_REQ) dbg_puts("LINK: OTA_REQ received\n");
            break;
        }
        case ALINK_FT_DETECT: {
            uint32_t lock = dbg_line_lock();
            dbg_puts("LINK DETECT from=");
            dbg_putu32(f->node_id);
            dbg_puts(" cls=");
            dbg_putu32(f->payload[0]);
            dbg_putc('\n');
            dbg_line_unlock(lock);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void acoustic_link_init(uint8_t node_id) {
    g_node_id = node_id;
    build_codes();

    for (uint32_t n = 0; n < ALINK_SAMPLES_CHIP; n++) {
        float a = 2.0f * (float)M_PI * (float)ALINK_PREAMBLE_HZ *
                  (float)n / (float)ALINK_FS_HZ;
        s_cos4[n] = cosf(a);
        s_sin4[n] = sinf(a);
    }
    for (uint32_t h = 0; h < ALINK_NHOPS; h++) {
        for (uint32_t n = 0; n < ALINK_SAMPLES_CHIP; n++) {
            float a = 2.0f * (float)M_PI * (float)g_hop_hz[h] *
                      (float)n / (float)ALINK_FS_HZ;
            s_hop_cos[h][n] = cosf(a);
            s_hop_sin[h][n] = sinf(a);
        }
    }

    s_chip_fill = 0;
    s_hist_head = 0;
    s_hist_fill = 0;
    rx_reset_search();
    s_rx_head = s_rx_tail = 0;
    s_next_beacon_us = 0;
    node_store_init();
}

void acoustic_link_set_node_id(uint8_t id) { g_node_id = id; }
uint8_t acoustic_link_node_id(void) { return g_node_id; }

bool acoustic_link_send_wifi_wake(uint8_t channel_token) {
    uint8_t p[ALINK_MAX_PAYLOAD];
    memset(p, 0, sizeof(p));
    p[0] = ALINK_CTRL_WIFI_WAKE;
    p[1] = channel_token;
    return acoustic_link_send(ALINK_FT_CTRL, 0u, p, ALINK_MAX_PAYLOAD);
}

uint32_t acoustic_link_tx_count(void) { return s_tx_count; }
uint32_t acoustic_link_rx_count(void) { return s_rx_count; }
uint32_t acoustic_link_rx_bad_crc(void) { return s_rx_bad_crc; }
bool acoustic_link_tx_busy(void) { return buzzer_tx_busy(); }
bool acoustic_link_wifi_wake_pending(void) { return s_wifi_wake_pending; }

uint32_t acoustic_link_frame_air_us(void) {
    return (uint32_t)ALINK_TX_CHIPS * (uint32_t)ALINK_CHIP_US;
}

void acoustic_link_status_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("LINK node=");
    dbg_putu32(g_node_id);
    dbg_puts(" ver=");
    dbg_putu32(ALINK_VERSION);
    dbg_puts(" air_ms=");
    dbg_putu32(acoustic_link_frame_air_us() / 1000u);
    dbg_puts(" tx=");
    dbg_putu32(s_tx_count);
    dbg_puts(" rx=");
    dbg_putu32(s_rx_count);
    dbg_puts(" bad_crc=");
    dbg_putu32(s_rx_bad_crc);
    dbg_puts(" peers=");
    dbg_putu32(node_store_count());
    dbg_puts(" wifi_wake=");
    dbg_puts(s_wifi_wake_pending ? "1" : "0");
    dbg_putc('\n');
    dbg_line_unlock(lock);
    node_store_list_uart();
}
