// acoustic_link.c — DSSS-BPSK node link on the GP6/GP7 4 kHz piezo + mic RX.
// See acoustic_link.h for the layering overview. No malloc; RX runs in bounded
// chunks from the main loop; TX hands a chip buffer to the buzzer ISR.
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

// ---------------------------------------------------------------------------
// Node identity + codes
// ---------------------------------------------------------------------------
static uint8_t g_node_id;

static int8_t g_pre_code[ALINK_MAX_NODES][ALINK_PREAMBLE_LEN];   // +1/-1
static int8_t g_data_code[ALINK_MAX_NODES][ALINK_DATA_SF];       // +1/-1

// Two maximal-length degree-7 LFSRs (a preferred-pair Gold construction):
//   seqA: x^7 + x^6 + 1   seqB: x^7 + x^3 + 1
static void build_mseq(uint8_t taps_hi, uint8_t taps_lo, uint8_t out[ALINK_PREAMBLE_LEN]) {
    uint8_t lfsr = 0x7Fu;
    for (uint32_t i = 0; i < ALINK_PREAMBLE_LEN; i++) {
        uint8_t bit = (uint8_t)(((lfsr >> taps_hi) ^ (lfsr >> taps_lo)) & 1u);
        out[i] = (uint8_t)(lfsr & 1u);
        lfsr = (uint8_t)(((lfsr << 1) | bit) & 0x7Fu);
    }
}

static void build_codes(void) {
    uint8_t a[ALINK_PREAMBLE_LEN], b[ALINK_PREAMBLE_LEN];
    build_mseq(6u, 5u, a);   // taps at register bits 6,5  -> x^7+x^6+1
    build_mseq(6u, 2u, b);   // taps at register bits 6,2  -> x^7+x^3+1
    for (uint32_t n = 0; n < ALINK_MAX_NODES; n++) {
        for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++) {
            uint8_t g = a[k] ^ b[(k + n) % ALINK_PREAMBLE_LEN];   // Gold family
            g_pre_code[n][k] = g ? -1 : +1;
        }
        for (uint32_t k = 0; k < ALINK_DATA_SF; k++)
            g_data_code[n][k] = g_pre_code[n][k];
    }
}

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, same poly as the flash stores)
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
// Hamming(7,4) FEC — single-error-correcting, per nibble
// ---------------------------------------------------------------------------
static void hamming_enc(uint8_t nib, uint8_t out[7]) {
    uint8_t d0 = (nib >> 0) & 1u, d1 = (nib >> 1) & 1u;
    uint8_t d2 = (nib >> 2) & 1u, d3 = (nib >> 3) & 1u;
    out[0] = (uint8_t)(d0 ^ d1 ^ d3);   // p1
    out[1] = (uint8_t)(d0 ^ d2 ^ d3);   // p2
    out[2] = d0;
    out[3] = (uint8_t)(d1 ^ d2 ^ d3);   // p3
    out[4] = d1;
    out[5] = d2;
    out[6] = d3;
}

static uint8_t hamming_dec(const uint8_t in[7]) {
    uint8_t p1 = in[0], p2 = in[1], d0 = in[2], p3 = in[3];
    uint8_t d1 = in[4], d2 = in[5], d3 = in[6];
    uint8_t s1 = (uint8_t)(p1 ^ d0 ^ d1 ^ d3);
    uint8_t s2 = (uint8_t)(p2 ^ d0 ^ d2 ^ d3);
    uint8_t s3 = (uint8_t)(p3 ^ d1 ^ d2 ^ d3);
    uint8_t syn = (uint8_t)(s1 | (s2 << 1) | (s3 << 2));
    if (syn) {
        // Syndrome (s1,s2,s3) -> codeword index (see hamming_enc layout):
        //   001->p1(0) 010->p2(1) 011->d0(2) 100->p3(3)
        //   101->d1(4) 110->d2(5) 111->d3(6)
        static const int synmap[8] = { -1, 0, 1, 2, 3, 4, 5, 6 };
        uint8_t v[7] = { in[0], in[1], in[2], in[3], in[4], in[5], in[6] };
        int p = synmap[syn & 7u];
        if (p >= 0) v[p] ^= 1u;
        d0 = v[2]; d1 = v[4]; d2 = v[5]; d3 = v[6];
    }
    return (uint8_t)(d0 | (d1 << 1) | (d2 << 2) | (d3 << 3));
}

// ---------------------------------------------------------------------------
// Crypto hook (reserved). Today a no-op pass-through; the frame already
// carries key_id/nonce/flags so enabling AEAD later needs no wire change.
// ---------------------------------------------------------------------------
static bool crypto_seal(uint8_t *payload, uint8_t len, uint8_t key_id, uint32_t nonce) {
    (void)payload; (void)len; (void)key_id; (void)nonce;
    return true;   // TODO: AEAD (encrypt-then-MAC), e.g. ChaCha20-Poly1305
}
static bool crypto_open(uint8_t *payload, uint8_t len, uint8_t key_id, uint32_t nonce) {
    (void)payload; (void)len; (void)key_id; (void)nonce;
    return true;
}

// ---------------------------------------------------------------------------
// TX
// ---------------------------------------------------------------------------
static uint8_t  s_tx_chips[ALINK_TX_CHIPS];
static uint8_t  s_tx_seq;
static uint32_t s_tx_count;
static uint8_t  s_pending_seq;        // seq of frame currently being emitted
static bool     s_pending_note;       // need to record accurate t1 once it starts
static uint64_t s_last_tx_start_us;

// Build the 434-bit Hamming-coded stream from 31 plaintext bytes, then spread.
static void build_tx_chips(const uint8_t frame[ALINK_FRAME_BYTES]) {
    uint8_t bits[ALINK_CODED_BITS];
    uint32_t bi = 0;
    for (uint32_t i = 0; i < ALINK_FRAME_BYTES; i++) {
        uint8_t hi = (uint8_t)(frame[i] >> 4);
        uint8_t lo = (uint8_t)(frame[i] & 0x0Fu);
        uint8_t cw[7];
        hamming_enc(hi, cw);
        for (int k = 0; k < 7; k++) bits[bi++] = cw[k];
        hamming_enc(lo, cw);
        for (int k = 0; k < 7; k++) bits[bi++] = cw[k];
    }

    uint32_t ci = 0;
    const int8_t *pre = g_pre_code[g_node_id % ALINK_MAX_NODES];
    const int8_t *dc  = g_data_code[g_node_id % ALINK_MAX_NODES];
    for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++)
        s_tx_chips[ci++] = (pre[k] < 0) ? 1u : 0u;   // amplitude -> phase bit
    for (uint32_t b = 0; b < ALINK_CODED_BITS; b++) {
        int amp_bit = (bits[b] ? -1 : +1);
        for (uint32_t k = 0; k < ALINK_DATA_SF; k++) {
            int amp = amp_bit * dc[k];              // (1-2b)*code
            s_tx_chips[ci++] = (amp < 0) ? 1u : 0u;
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

    // Seal payload region in place (stub today).
    if (flags & ALINK_FLAG_ENCRYPTED)
        (void)crypto_seal(&frame[11], ALINK_MAX_PAYLOAD, key_id, nonce);

    uint32_t crc = crc32(frame, 27);
    frame[27] = (uint8_t)(crc & 0xFF);
    frame[28] = (uint8_t)((crc >> 8) & 0xFF);
    frame[29] = (uint8_t)((crc >> 16) & 0xFF);
    frame[30] = (uint8_t)((crc >> 24) & 0xFF);

    build_tx_chips(frame);
    if (!buzzer_tx_chips(s_tx_chips, ALINK_TX_CHIPS)) return false;

    s_pending_seq = s_tx_seq;
    s_pending_note = (type == ALINK_FT_BEACON);
    s_tx_seq++;
    s_tx_count++;
    return true;
}

// ---------------------------------------------------------------------------
// Beacon scheduling (slotted-ALOHA-ish: periodic + random jitter)
// ---------------------------------------------------------------------------
#define BEACON_BASE_MS   1500u
#define BEACON_JITTER_MS 500u
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
    if (buzzer_tx_busy()) return;   // listen/hold: don't stomp our own PHY

    uint8_t p[ALINK_MAX_PAYLOAD];
    memset(p, 0, sizeof(p));
    uint32_t tx_mono = (uint32_t)now;              // approx send time (low32)
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
// RX ring (SPSC: producer = USB frame builder, consumer = poll)
// ---------------------------------------------------------------------------
#define RX_RING 8192u
static int16_t          s_rx_ring[RX_RING];
static volatile uint32_t s_rx_head;   // producer
static volatile uint32_t s_rx_tail;   // consumer

void acoustic_link_rx_push(int16_t mono) {
    uint32_t h = s_rx_head;
    uint32_t nh = (h + 1u) & (RX_RING - 1u);
    if (nh == s_rx_tail) return;       // full: drop (bounded, non-blocking)
    s_rx_ring[h] = mono;
    s_rx_head = nh;
}

// ---------------------------------------------------------------------------
// RX matched filter / despreader
// ---------------------------------------------------------------------------
static float s_cos_lut[ALINK_SAMPLES_CYCLE];
static float s_sin_lut[ALINK_SAMPLES_CYCLE];
static uint32_t s_smp_phase;

// Integrate-and-dump chip accumulator.
static float s_i_acc, s_q_acc;
static uint32_t s_chip_smp;

// Preamble chip history (circular).
static float s_hist_i[ALINK_PREAMBLE_LEN];
static float s_hist_q[ALINK_PREAMBLE_LEN];
static uint32_t s_hist_head;
static uint32_t s_hist_fill;

// Receiver state machine.
enum { RX_SEARCH = 0, RX_DATA };
static uint8_t  s_rx_state;
static uint8_t  s_rx_node;       // detected sender code index
static float    s_rx_cos, s_rx_sin;  // channel phase reference
static uint64_t s_rx_toa_us;
static float    s_rx_q;
static uint32_t s_bit_idx;
static uint32_t s_chip_in_bit;
static float    s_bit_i, s_bit_q;
static uint8_t  s_coded[ALINK_CODED_BITS];

static uint32_t s_rx_count;
static uint32_t s_rx_bad_crc;
static volatile bool s_wifi_wake_pending;

#define DETECT_RHO   0.30f     // normalized correlation threshold
#define DETECT_EMIN  1.0e6f    // minimum window energy to consider a peak

static void rx_reset_search(void) {
    s_rx_state = RX_SEARCH;
    s_bit_idx = 0;
    s_chip_in_bit = 0;
    s_bit_i = s_bit_q = 0.0f;
}

static void dispatch_frame(const alink_frame_t *f);

static void finalize_frame(void) {
    uint8_t bytes[ALINK_FRAME_BYTES];
    for (uint32_t i = 0; i < ALINK_FRAME_BYTES; i++) {
        uint8_t hi = hamming_dec(&s_coded[(i * 2u) * 7u]);
        uint8_t lo = hamming_dec(&s_coded[(i * 2u + 1u) * 7u]);
        bytes[i] = (uint8_t)((hi << 4) | (lo & 0x0Fu));
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
    s_rx_count++;
    dispatch_frame(&f);
    rx_reset_search();
}

// One completed chip (complex) at local time chip_us.
static void rx_on_chip(float ci, float cq, uint64_t chip_us) {
    if (s_rx_state == RX_DATA) {
        const int8_t *dc = g_data_code[s_rx_node];
        s_bit_i += ci * (float)dc[s_chip_in_bit];
        s_bit_q += cq * (float)dc[s_chip_in_bit];
        if (++s_chip_in_bit >= ALINK_DATA_SF) {
            float val = s_bit_i * s_rx_cos + s_bit_q * s_rx_sin;  // derotate
            s_coded[s_bit_idx] = (val < 0.0f) ? 1u : 0u;
            s_bit_idx++;
            s_chip_in_bit = 0;
            s_bit_i = s_bit_q = 0.0f;
            if (s_bit_idx >= ALINK_CODED_BITS) finalize_frame();
        }
        return;
    }

    // SEARCH: push chip into history, correlate against candidate codes.
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
        // oldest chip is at s_hist_head (just overwritten pos is newest-1);
        // align code[0] with oldest sample.
        uint32_t idx = s_hist_head;
        for (uint32_t k = 0; k < ALINK_PREAMBLE_LEN; k++) {
            float c = (float)code[k];
            ri += c * s_hist_i[idx];
            rq += c * s_hist_q[idx];
            idx = (idx + 1u) % ALINK_PREAMBLE_LEN;
        }
        float mag2 = ri * ri + rq * rq;
        if (mag2 > best_mag2) { best_mag2 = mag2; best_node = (int)n; best_ci = ri; best_cq = rq; }
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
        s_chip_in_bit = 0;
        s_bit_i = s_bit_q = 0.0f;
        // Clear history so the next search starts fresh after this frame.
        s_hist_fill = 0;
    }
}

void acoustic_link_poll(void) {
    schedule_tx();

    // Record the accurate preamble start time for our outstanding beacon poll.
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
    uint32_t avail = (head - tail) & (RX_RING - 1u);
    if (avail == 0u) return;

    uint64_t now = time_us_64();
    uint32_t remaining = avail;
    while (tail != head) {
        int16_t x = s_rx_ring[tail];
        tail = (tail + 1u) & (RX_RING - 1u);
        remaining--;

        float xf = (float)x;
        uint32_t p = s_smp_phase;
        s_i_acc += xf * s_cos_lut[p];
        s_q_acc += xf * s_sin_lut[p];
        s_smp_phase = (p + 1u >= ALINK_SAMPLES_CYCLE) ? 0u : (p + 1u);

        if (++s_chip_smp >= ALINK_SAMPLES_CHIP) {
            uint64_t chip_us = now - (uint64_t)remaining * 1000000ull / ALINK_FS_HZ;
            rx_on_chip(s_i_acc, s_q_acc, chip_us);
            s_i_acc = s_q_acc = 0.0f;
            s_chip_smp = 0;
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
    // Actual STA connect is deferred: it needs SSID/credential provisioning and
    // must coexist with the BTstack/RID cyw43_arch instance. The application
    // layer polls acoustic_link_wifi_wake_pending() to drive the connect + OTA.
}
#else
static void wifi_wake(uint8_t token) {
    (void)token;
    s_wifi_wake_pending = true;   // recorded even on non-wireless boards
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

            // Coarse time adoption from a synced peer when we are unsynced.
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
    for (uint32_t i = 0; i < ALINK_SAMPLES_CYCLE; i++) {
        float a = 2.0f * 3.14159265f * (float)i / (float)ALINK_SAMPLES_CYCLE;
        s_cos_lut[i] = cosf(a);
        s_sin_lut[i] = sinf(a);
    }
    s_smp_phase = 0;
    s_i_acc = s_q_acc = 0.0f;
    s_chip_smp = 0;
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

void acoustic_link_status_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("LINK node=");
    dbg_putu32(g_node_id);
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
