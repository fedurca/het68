// doa.c — 3D direction-of-arrival on core1. See doa.h.
//
// core0 pushes each 6-channel frame via doa_ring_push() from the USB/I2S feed.
// core1 consumes windows, estimates direction by time-domain cross-correlation
// (TDOA) and prints azimuth/elevation on the debug UART.
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
// Array geometry — 512 mm cube on vertex (see README).
// ---------------------------------------------------------------------------
#define DOA_EDGE_M      0.512f
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

#define DOA_FS           48000.0f
#define DOA_C_SOUND      343.0f
#define DOA_N            256u
#define DOA_MAXLAG       72
#define DOA_OUT_SAMPLES  9600u
#define DOA_RMS_ACTIVE   4.0f
#define DOA_TDOA_SIGN    (+1.0f)

// SPSC ring: core0 producer, core1 consumer.
#define DOA_RING_SZ     2048u
#define DOA_RING_MASK   (DOA_RING_SZ - 1u)
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
        for (uint32_t n = DOA_MAXLAG; n < DOA_N - DOA_MAXLAG; n++) s += a[n] * b[n + lag];
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
    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;
        float prev = 0.0f, e = 0.0f, raw_e = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            raw_e += x * x;
            float y = x - 0.97f * prev;
            prev = x;
            g_work[c][n] = y;
            e += y * y;
        }
        g_energy[c] = e;
        active[c] = (sqrtf(raw_e / (float)DOA_N) > DOA_RMS_ACTIVE);
        if (active[c]) nactive++;
    }
    g_doa_nactive = (uint32_t)nactive;

    if (nactive < 4) {
        uint32_t s = dbg_line_lock();
        dbg_puts("DOA: insufficient mics active=");
        dbg_putu32((uint32_t)nactive);
        dbg_putc('\n');
        dbg_line_unlock(s);
        return;
    }

    int ref = -1;
    for (int c = 0; c < 6; c++) if (active[c]) { ref = c; break; }
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
    float rms_ref = sqrtf(eref / (float)DOA_N);
    float dbfs = 20.0f * log10f((rms_ref + 1e-6f) / 32768.0f);

    uint32_t s = dbg_line_lock();
    dbg_puts("DOA az=");   put_f1(az);
    dbg_puts(" el=");      put_f1(el);
    dbg_puts(" conf=");    put_f1(conf);
    dbg_puts(" lvl=");     put_f1(dbfs);
    dbg_puts("dB ref=");   dbg_putu32((uint32_t)ref);
    dbg_puts(" pairs=");   dbg_putu32((uint32_t)conf_n);
    dbg_putc('\n');
    dbg_line_unlock(s);
    g_doa_out++;
}

static void doa_core1_main(void) {
    het68_core1_setup();

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
    het68_launch_core1(doa_core1_main);
}
