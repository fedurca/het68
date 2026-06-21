// doa.c — 3D direction-of-arrival estimator. See doa.h.
//
// IMPORTANT — why this runs on core0, not core1:
// On this RP2350 board a core launched via multicore_launch_core1 reliably
// stops executing ~300 us after launch, regardless of what it runs (verified
// with integer-only and FP loops, in flash and in RAM, with IRQs on and off,
// and launched before any core0 peripheral init). core0 is perfectly healthy,
// so the analysis runs there instead. To keep the 1 ms USB audio cadence
// intact (audio is fed from tud_task() in the main loop), doa_service() does a
// strictly bounded amount of work per call — at most one microphone-pair
// cross-correlation — and the main loop interleaves tud_task() between calls.
#include "doa.h"
#include "debug_io.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Array geometry
// ---------------------------------------------------------------------------
// The cube stands on a vertex, so its body diagonal is vertical. The three
// "upper" faces (mics 1-3) point up at +35.26° elevation, 120° apart in
// azimuth; the three "lower" faces (mics 4-6) point down at -35.26°, azimuths
// interleaved 60° between the upper ones. World frame: X=north, Y=east, Z=up.
// Azimuth is compass degrees, clockwise from north (= atan2(east, north)).
//
// Mic 1 faces north. Each microphone sits at the centre of its face, i.e. at
// (edge/2) along the (unit) face normal. Channel order from the I2S wiring is
// mic1->ch0, mic2->ch1, ... mic6->ch5. Each lower mic (4-6) is the face
// directly opposite a top mic (1-3).
//
// Unit face normals (north, east, up):
//   c = sqrt(2/3) = 0.81650 (horizontal magnitude of the "north" top normal)
//   s = 1/sqrt(3) = 0.57735 (vertical component)
//   c*sin(120) = 0.70711, c*cos(120) = -0.40825
#define DOA_EDGE_M      0.512f
#define DOA_FACE_R      (DOA_EDGE_M * 0.5f)     // face-centre radius = edge/2

static const float MIC_DIR[6][3] = {
    {  0.81650f,  0.00000f,  0.57735f },  // ch0 mic1  top    az   0
    { -0.40825f,  0.70711f,  0.57735f },  // ch1 mic2  top    az 120
    { -0.40825f, -0.70711f,  0.57735f },  // ch2 mic3  top    az 240
    { -0.81650f,  0.00000f, -0.57735f },  // ch3 mic4  bottom az 180
    {  0.40825f, -0.70711f, -0.57735f },  // ch4 mic5  bottom az 300
    {  0.40825f,  0.70711f, -0.57735f },  // ch5 mic6  bottom az  60
};

// Microphone positions in metres (world frame).
static float MIC_POS[6][3];

// ---------------------------------------------------------------------------
// Analysis parameters
// ---------------------------------------------------------------------------
#define DOA_FS           48000.0f
#define DOA_C_SOUND      343.0f       // speed of sound (m/s, ~20°C)
#define DOA_N            256u         // analysis window (samples, ~5.3 ms)
#define DOA_MAXLAG       72           // >= max TDOA (512mm/343 = 1.49 ms = 72 smp)
#define DOA_OUT_SAMPLES  9600u        // run cadence in samples (~200 ms)
#define DOA_RMS_ACTIVE   4.0f         // per-channel RMS to count a mic as live
// Sign of the TDOA->geometry mapping. Derivation gives +1; kept as a knob so a
// lab test against a known source can flip it without touching the math.
#define DOA_TDOA_SIGN    (+1.0f)

// ---------------------------------------------------------------------------
// SPSC ring. Producer: usb feed (build_usb_frame_from_i2s). Consumer:
// doa_service(). Both run on core0 and never preempt each other, so this is a
// plain ring; the barrier is kept for clarity/portability.
// ---------------------------------------------------------------------------
#define DOA_RING_SZ     2048u
#define DOA_RING_MASK   (DOA_RING_SZ - 1u)
static volatile int16_t  g_ring[DOA_RING_SZ][6];
static volatile uint32_t g_head;     // next write index (monotonic)

// Diagnostics (read from the heartbeat).
volatile uint32_t g_doa_out;         // DOA result lines emitted
volatile uint32_t g_doa_nactive;     // last active-mic count

void doa_ring_push(const int16_t s6[6]) {
    uint32_t h = g_head;
    int16_t *slot = (int16_t *)g_ring[h & DOA_RING_MASK];
    for (int i = 0; i < 6; i++) slot[i] = s6[i];
    __dmb();
    g_head = h + 1u;
}

// ---------------------------------------------------------------------------
// Small float helpers
// ---------------------------------------------------------------------------
static void put_f1(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 10.0f + 0.5f);
    if (fp >= 10u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    dbg_putu32(fp);
}

// Solve a 3x3 system M x = b by Cramer's rule. Returns false if near-singular.
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
// Bounded state machine (one slice of work per doa_service() call)
// ---------------------------------------------------------------------------
enum { ST_IDLE = 0, ST_XCORR, ST_SOLVE };

static int      s_state = ST_IDLE;
static uint32_t s_last_proc;
static float    s_work[6][DOA_N];   // mean-removed, pre-emphasised window
static float    s_energy[6];
static int      s_ref;
static float    s_eref;
static int      s_pairs[6];         // active channels (excluding ref)
static int      s_npairs;
static int      s_pidx;
static float    s_AtA[3][3];
static float    s_Atb[3];
static float    s_conf_sum;

// Copy newest DOA_N frames (ending at head h-1), de-interleaved, then per
// channel remove DC, apply 1st-order pre-emphasis (whitening) and measure
// energy. Picks the reference and the active-channel list.
static void doa_prep(uint32_t h) {
    uint32_t start = h - DOA_N;
    for (uint32_t n = 0; n < DOA_N; n++) {
        uint32_t idx = (start + n) & DOA_RING_MASK;
        for (int c = 0; c < 6; c++) s_work[c][n] = (float)g_ring[idx][c];
    }

    int nactive = 0;
    bool active[6];
    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += s_work[c][n];
        mean /= (float)DOA_N;
        float prev = 0.0f, e = 0.0f, raw_e = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = s_work[c][n] - mean;
            raw_e += x * x;
            float y = x - 0.97f * prev;
            prev = x;
            s_work[c][n] = y;
            e += y * y;
        }
        s_energy[c] = e;
        active[c] = (sqrtf(raw_e / (float)DOA_N) > DOA_RMS_ACTIVE);
        if (active[c]) nactive++;
    }
    g_doa_nactive = (uint32_t)nactive;

    s_ref = -1;
    s_npairs = 0;
    for (int c = 0; c < 6; c++) {
        if (!active[c]) continue;
        if (s_ref < 0) { s_ref = c; continue; }
        s_pairs[s_npairs++] = c;
    }
    s_eref = (s_ref >= 0) ? s_energy[s_ref] : 0.0f;

    for (int r = 0; r < 3; r++) { s_Atb[r] = 0.0f; for (int cc = 0; cc < 3; cc++) s_AtA[r][cc] = 0.0f; }
    s_conf_sum = 0.0f;
    s_pidx = 0;
}

// Cross-correlate channel i against ref; fractional peak lag (+ve = i later),
// normalised peak height in *conf.
static float xcorr_delay(int ref, int i, float *conf) {
    const float *a = s_work[ref];
    const float *b = s_work[i];
    float best = -1e30f;
    int   bestlag = 0;
    float corr[2 * DOA_MAXLAG + 1];
    for (int lag = -DOA_MAXLAG; lag <= DOA_MAXLAG; lag++) {
        float s = 0.0f;
        for (uint32_t n = DOA_MAXLAG; n < DOA_N - DOA_MAXLAG; n++) s += a[n] * b[n + lag];
        corr[lag + DOA_MAXLAG] = s;
        if (s > best) { best = s; bestlag = lag; }
    }
    float lagf = (float)bestlag;
    if (bestlag > -DOA_MAXLAG && bestlag < DOA_MAXLAG) {
        float y1 = corr[bestlag + DOA_MAXLAG - 1];
        float y2 = corr[bestlag + DOA_MAXLAG];
        float y3 = corr[bestlag + DOA_MAXLAG + 1];
        float denom = (y1 - 2.0f * y2 + y3);
        if (fabsf(denom) > 1e-12f) {
            float d = 0.5f * (y1 - y3) / denom;
            if (d > -1.0f && d < 1.0f) lagf += d;
        }
    }
    float norm = sqrtf(s_eref * s_energy[i]) + 1e-6f;
    float c = best / norm;
    *conf = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return lagf;
}

static void doa_solve_and_emit(void) {
    float d[3];
    if (!solve3(s_AtA, s_Atb, d)) {
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
    float conf = s_npairs ? s_conf_sum / (float)s_npairs : 0.0f;
    float rms_ref = sqrtf(s_eref / (float)DOA_N);
    float dbfs = 20.0f * log10f((rms_ref + 1e-6f) / 32768.0f);

    uint32_t s = dbg_line_lock();
    dbg_puts("DOA az=");   put_f1(az);
    dbg_puts(" el=");      put_f1(el);
    dbg_puts(" conf=");    put_f1(conf);
    dbg_puts(" lvl=");     put_f1(dbfs);
    dbg_puts("dB ref=");   dbg_putu32((uint32_t)s_ref);
    dbg_puts(" pairs=");   dbg_putu32((uint32_t)s_npairs);
    dbg_putc('\n');
    dbg_line_unlock(s);
    g_doa_out++;
}

void doa_init(void) {
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 3; k++) MIC_POS[i][k] = MIC_DIR[i][k] * DOA_FACE_R;
    s_last_proc = g_head;
    s_state = ST_IDLE;
}

void doa_service(void) {
    switch (s_state) {
    case ST_IDLE: {
        uint32_t h = g_head;
        if ((uint32_t)(h - s_last_proc) < DOA_OUT_SAMPLES) return;
        s_last_proc = h;
        doa_prep(h);
        if (s_ref < 0 || s_npairs < 3) {
            uint32_t s = dbg_line_lock();
            dbg_puts("DOA: insufficient mics active=");
            dbg_putu32(g_doa_nactive);
            dbg_putc('\n');
            dbg_line_unlock(s);
            s_state = ST_IDLE;
            return;
        }
        s_state = ST_XCORR;
        return;
    }
    case ST_XCORR: {
        int i = s_pairs[s_pidx];
        float conf = 0.0f;
        float lag = xcorr_delay(s_ref, i, &conf);
        float row[3] = {
            MIC_POS[i][0] - MIC_POS[s_ref][0],
            MIC_POS[i][1] - MIC_POS[s_ref][1],
            MIC_POS[i][2] - MIC_POS[s_ref][2],
        };
        float bb = DOA_TDOA_SIGN * (-DOA_C_SOUND / DOA_FS) * lag;   // metres
        float w = conf;
        for (int r = 0; r < 3; r++) {
            for (int cc = 0; cc < 3; cc++) s_AtA[r][cc] += w * row[r] * row[cc];
            s_Atb[r] += w * row[r] * bb;
        }
        s_conf_sum += conf;
        if (++s_pidx >= s_npairs) s_state = ST_SOLVE;
        return;
    }
    case ST_SOLVE:
    default:
        doa_solve_and_emit();
        s_state = ST_IDLE;
        return;
    }
}
