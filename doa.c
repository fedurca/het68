// doa.c — 3D direction-of-arrival on core1. See doa.h.
//
// core0 pushes each 6-channel frame via doa_ring_push() from the USB/I2S feed.
// core1 consumes windows, bandpass-filters into the drone-relevant band
// (rejecting wind-dominated LF), estimates direction by time-domain
// cross-correlation (TDOA) and prints azimuth/elevation on the debug UART.
//
// core1 must be launched with het68_launch_core1() (see core1_launch.c): after
// OpenOCD SWD flash, multicore_launch_core1() alone leaves core1 in a bad state.
#include "doa.h"
#include "core1_launch.h"
#include "debug_io.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Array geometry — cube standing on a vertex, mics at the six face centres
// (an octahedron). See README and array_cube_design.md.
//
// DOA_EDGE_MM is the ONE knob for the array size: DOA_MAXLAG and the comparison
// window below derive from it, so the same firmware can drive any integer edge
// length (e.g. 50 / 100 / 128 / 150 / 200 / 256 / 512 / 1024 mm). MIC_DIR stays
// the same — only the scale changes. Edge is an integer in millimetres so the
// derivation is a valid integer constant expression usable in the #if tiers.
//
// Override at build time (default 512 mm):  HET68_DOA_EDGE_MM=150 ./build.sh
// ---------------------------------------------------------------------------
#ifdef HET68_DOA_EDGE_MM
#define DOA_EDGE_MM     HET68_DOA_EDGE_MM
#else
#define DOA_EDGE_MM     512
#endif
#define DOA_EDGE_M      (DOA_EDGE_MM * 0.001f)
#define DOA_FACE_R      (DOA_EDGE_M * 0.5f)

static const float MIC_DIR[6][3] = {
    {  0.81650f,  0.00000f,  0.57735f },
    { -0.40825f,  0.70711f,  0.57735f },
    { -0.40825f, -0.70711f,  0.57735f },
    { -0.81650f,  0.00000f, -0.57735f },
    {  0.40825f, -0.70711f, -0.57735f },
    {  0.40825f,  0.70711f, -0.57735f },
};

static float MIC_POS[6][3];

// Sample rate and speed of sound, kept in one place as both integer forms (for
// the DOA_MAXLAG / #if derivation) and float forms (for the estimator maths).
#define DOA_FS_HZ        48000
#define DOA_C_MM_S       343000
#define DOA_FS           ((float)DOA_FS_HZ)
#define DOA_C_SOUND      (DOA_C_MM_S * 0.001f)

// Longest baseline = full edge (opposite faces). One integer sample of TDOA is
// c/Fs = 343000/48000 = 7.15 mm of path difference, so cover ceil(edge / 7.15)
// samples plus 2 for the sub-sample parabolic interpolation headroom.
#define DOA_MAXLAG       ((DOA_EDGE_MM * DOA_FS_HZ + DOA_C_MM_S - 1) / DOA_C_MM_S + 2)

// Comparison window. The GCC core correlates over DOA_N - 2*DOA_MAXLAG samples,
// so the window must grow with the cube — a bigger array means a longer maximum
// delay to search. Auto-pick the smallest power-of-two window that keeps the
// usable span at least as long as the lag search (DOA_N >= 3*DOA_MAXLAG). This
// is the only DOA buffer that scales with edge length (g_work = 6*DOA_N floats,
// i.e. 24*DOA_N bytes); the ring buffer is fixed. Edge crossovers: 256 samples
// up to ~600 mm, 512 up to ~1200 mm, 1024 up to ~2400 mm.
#if   DOA_MAXLAG <= 85
#define DOA_N            256u
#elif DOA_MAXLAG <= 170
#define DOA_N            512u
#elif DOA_MAXLAG <= 341
#define DOA_N            1024u
#else
#error "DOA_EDGE_MM too large: add a larger DOA_N tier in doa.c"
#endif

#define DOA_OUT_SAMPLES  9600u
// Bandpass-domain RMS gate (int16 scale). Lower than the old broadband gate
// because wind LF is removed before the threshold.
#define DOA_RMS_ACTIVE   2.5f
#define DOA_TDOA_SIGN    (+1.0f)

// Skip filter transient at the start of each analysis window (~1 ms @ 48 kHz).
#define DOA_FILT_SETTLE  48u
#define DOA_CORR_LO      (DOA_MAXLAG + DOA_FILT_SETTLE)
#define DOA_CORR_HI      (DOA_N - DOA_MAXLAG)
#if DOA_CORR_LO >= DOA_CORR_HI
#error "DOA window too small for filter settle + lag search"
#endif

// Wind vs drone energy gate: require bandpass energy >= ratio * wind-band energy.
// Wind is concentrated well below ~300 Hz; multirotor prop/motor content is
// typically strong from ~0.8–6 kHz (blade-pass harmonics + broadband hiss).
#define DOA_WIND_RATIO   0.35f

// SPSC ring: core0 producer, core1 consumer.
#define DOA_RING_SZ     2048u
#define DOA_RING_MASK   (DOA_RING_SZ - 1u)
// The consumer reads the most recent DOA_N samples while the producer keeps
// writing; keep at least 2x headroom so a window is never overwritten mid-read.
#if DOA_RING_SZ < 2u * DOA_N
#error "DOA_RING_SZ too small for DOA_N: increase DOA_RING_SZ"
#endif
static volatile int16_t  g_ring[DOA_RING_SZ][6];
static volatile uint32_t g_head;

volatile uint32_t g_doa_out;
volatile uint32_t g_doa_nactive;
volatile uint32_t g_doa_iter;

void doa_ring_push(const int16_t s6[6]) {
    uint32_t h = g_head;
    int16_t *slot = (int16_t *)g_ring[h & DOA_RING_MASK];
    for (int i = 0; i < 6; i++) slot[i] = s6[i];
    __dmb();
    g_head = h + 1u;
}

static void put_f1(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 10.0f + 0.5f);
    if (fp >= 10u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    dbg_putu32(fp);
}

static bool solve3(const float M[3][3], const float b[3], float x[3]) {
    float det =
        M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
        M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
        M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (fabsf(det) < 1e-9f) return false;
    float inv = 1.0f / det;
    x[0] = inv * (b[0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                  M[0][1] * (b[1] * M[2][2] - M[1][2] * b[2]) +
                  M[0][2] * (b[1] * M[2][1] - M[1][1] * b[2]));
    x[1] = inv * (M[0][0] * (b[1] * M[2][2] - M[1][2] * b[2]) -
                  b[0] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                  M[0][2] * (M[1][0] * b[2] - b[1] * M[2][0]));
    x[2] = inv * (M[0][0] * (M[1][1] * b[2] - b[1] * M[2][1]) -
                  M[0][1] * (M[1][0] * b[2] - b[1] * M[2][0]) +
                  b[0] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]));
    return true;
}

// ---------------------------------------------------------------------------
// Fixed biquads @ 48 kHz (Butterworth 2nd-order, bilinear). No malloc; DF2T.
// Drone band ≈ 800 Hz–6 kHz (prop BPF harmonics + motor hiss).
// Wind monitor ≈ DC–250 Hz.
// ---------------------------------------------------------------------------
typedef struct {
    float b0, b1, b2, a1, a2;
} biquad_coef_t;

typedef struct {
    float z1, z2;
} biquad_mem_t;

// HPF 800 Hz
static const biquad_coef_t k_hpf800 = {
    9.2862377786e-01f, -1.8572475557e+00f, 9.2862377786e-01f,
    -1.8521464854e+00f, 8.6234862603e-01f
};
// LPF 6000 Hz
static const biquad_coef_t k_lpf6000 = {
    9.7631072938e-02f, 1.9526214588e-01f, 9.7631072938e-02f,
    -9.4280904158e-01f, 3.3333333333e-01f
};
// LPF 250 Hz (wind / rumble energy probe)
static const biquad_coef_t k_lpf250 = {
    2.6165269507e-04f, 5.2330539013e-04f, 2.6165269507e-04f,
    -1.9537279491e+00f, 9.5477455992e-01f
};

static inline float biquad_step(const biquad_coef_t *c, biquad_mem_t *s, float x) {
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

static float g_work[6][DOA_N];
static float g_energy[6];
static float s_corr[2 * DOA_MAXLAG + 1];

static float xcorr_delay(int ref, int i, float eref, float *conf) {
    const float *a = g_work[ref];
    const float *b = g_work[i];
    float best = -1e30f;
    int   bestlag = 0;
    for (int lag = -DOA_MAXLAG; lag <= DOA_MAXLAG; lag++) {
        float s = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) s += a[n] * b[n + lag];
        s_corr[lag + DOA_MAXLAG] = s;
        if (s > best) { best = s; bestlag = lag; }
    }
    float lagf = (float)bestlag;
    if (bestlag > -DOA_MAXLAG && bestlag < DOA_MAXLAG) {
        float y1 = s_corr[bestlag + DOA_MAXLAG - 1];
        float y2 = s_corr[bestlag + DOA_MAXLAG];
        float y3 = s_corr[bestlag + DOA_MAXLAG + 1];
        float denom = (y1 - 2.0f * y2 + y3);
        if (fabsf(denom) > 1e-12f) {
            float d = 0.5f * (y1 - y3) / denom;
            if (d > -1.0f && d < 1.0f) lagf += d;
        }
    }
    // Normalize by the same lag-trimmed energy used in the correlation sum.
    float norm = sqrtf(eref * g_energy[i]) + 1e-6f;
    float c = best / norm;
    *conf = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return lagf;
}

static void doa_process(uint32_t h) {
    uint32_t start = h - DOA_N;
    for (uint32_t n = 0; n < DOA_N; n++) {
        uint32_t idx = (start + n) & DOA_RING_MASK;
        for (int c = 0; c < 6; c++) g_work[c][n] = (float)g_ring[idx][c];
    }

    int nactive = 0;
    bool active[6];
    float wind_ratio_sum = 0.0f;
    int wind_ratio_n = 0;

    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;

        biquad_mem_t hp = {0}, lp = {0}, wind = {0};
        float e_bp = 0.0f, e_wind = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            float w = biquad_step(&k_lpf250, &wind, x);
            float y = biquad_step(&k_hpf800, &hp, x);
            y = biquad_step(&k_lpf6000, &lp, y);
            g_work[c][n] = y;
            if (n >= DOA_FILT_SETTLE) {
                e_bp += y * y;
                e_wind += w * w;
            }
        }

        // Correlation energy over the same trimmed span as xcorr_delay.
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) {
            float y = g_work[c][n];
            e_corr += y * y;
        }
        g_energy[c] = e_corr;

        uint32_t n_power = DOA_N - DOA_FILT_SETTLE;
        float rms_bp = sqrtf(e_bp / (float)n_power);
        bool loud = rms_bp > DOA_RMS_ACTIVE;
        bool not_wind = e_bp >= DOA_WIND_RATIO * (e_wind + 1e-6f);
        active[c] = loud && not_wind;
        if (active[c]) nactive++;
        if (e_wind > 1e-3f) {
            wind_ratio_sum += e_bp / e_wind;
            wind_ratio_n++;
        }
    }
    g_doa_nactive = (uint32_t)nactive;

    if (nactive < 4) {
        uint32_t s = dbg_line_lock();
        dbg_puts("DOA: insufficient mics active=");
        dbg_putu32((uint32_t)nactive);
        dbg_puts(" (wind/band gate)\n");
        dbg_line_unlock(s);
        return;
    }

    int ref = -1;
    float eref_best = -1.0f;
    for (int c = 0; c < 6; c++) {
        if (!active[c]) continue;
        if (g_energy[c] > eref_best) {
            eref_best = g_energy[c];
            ref = c;
        }
    }
    float eref = g_energy[ref];

    float AtA[3][3] = {{0}};
    float Atb[3] = {0};
    float conf_sum = 0.0f;
    int conf_n = 0;
    for (int i = 0; i < 6; i++) {
        if (i == ref || !active[i]) continue;
        float conf = 0.0f;
        float lag = xcorr_delay(ref, i, eref, &conf);
        float row[3] = {
            MIC_POS[i][0] - MIC_POS[ref][0],
            MIC_POS[i][1] - MIC_POS[ref][1],
            MIC_POS[i][2] - MIC_POS[ref][2],
        };
        float bb = DOA_TDOA_SIGN * (-DOA_C_SOUND / DOA_FS) * lag;
        float w = conf;
        for (int r = 0; r < 3; r++) {
            for (int cc = 0; cc < 3; cc++) AtA[r][cc] += w * row[r] * row[cc];
            Atb[r] += w * row[r] * bb;
        }
        conf_sum += conf;
        conf_n++;
    }

    float d[3];
    if (!solve3(AtA, Atb, d)) {
        uint32_t s = dbg_line_lock();
        dbg_puts("DOA: singular geometry\n");
        dbg_line_unlock(s);
        return;
    }
    float mag = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (mag < 1e-6f) return;
    d[0] /= mag; d[1] /= mag; d[2] /= mag;

    float az = atan2f(d[1], d[0]) * (180.0f / 3.14159265f);
    if (az < 0.0f) az += 360.0f;
    float el = asinf(d[2]) * (180.0f / 3.14159265f);
    float conf = conf_n ? conf_sum / (float)conf_n : 0.0f;
    float rms_ref = sqrtf(eref / (float)(DOA_CORR_HI - DOA_CORR_LO));
    float dbfs = 20.0f * log10f((rms_ref + 1e-6f) / 32768.0f);
    float wrat = wind_ratio_n ? (wind_ratio_sum / (float)wind_ratio_n) : 0.0f;

    uint32_t s = dbg_line_lock();
    dbg_puts("DOA az=");   put_f1(az);
    dbg_puts(" el=");      put_f1(el);
    dbg_puts(" conf=");    put_f1(conf);
    dbg_puts(" lvl=");     put_f1(dbfs);
    dbg_puts("dB wrat=");  put_f1(wrat);
    dbg_puts(" ref=");     dbg_putu32((uint32_t)ref);
    dbg_puts(" pairs=");   dbg_putu32((uint32_t)conf_n);
    dbg_putc('\n');
    dbg_line_unlock(s);
    g_doa_out++;
}

static bool doa_core1_verify(void) {
    extern volatile uint32_t g_doa_iter;
    if (!g_core1_alive) {
        return false;
    }
    uint32_t t0 = g_doa_iter;
    sleep_ms(80);
    uint32_t t1 = g_doa_iter;
    if ((t1 - t0) < 50000u) {
        return false;
    }
    sleep_ms(80);
    return (g_doa_iter - t1) >= 50000u;
}

static void doa_core1_main(void) {
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 3; k++) MIC_POS[i][k] = MIC_DIR[i][k] * DOA_FACE_R;

    uint32_t last_proc = g_head;
    for (;;) {
        g_doa_iter++;
        uint32_t h = g_head;
        if ((uint32_t)(h - last_proc) < DOA_OUT_SAMPLES) {
            tight_loop_contents();
            continue;
        }
        last_proc = h;
        doa_process(h);
    }
}

void doa_start(void) {
    if (!het68_launch_core1_verify(doa_core1_main, doa_core1_verify)) {
        uint32_t s = dbg_line_lock();
        dbg_puts("DOA: core1 launch FAILED\n");
        dbg_line_unlock(s);
    }
}
