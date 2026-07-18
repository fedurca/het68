// doa.c — multi-source 3D DOA on core1 with drone vs human classification.
//
// core0 pushes each 6-channel frame via doa_ring_push() from the USB/I2S feed.
// core1 runs two parallel acoustic front-ends:
//   • drone band  (~800 Hz–6 kHz): continuous TDOA, up to 2 tracks (primary + SIC)
//   • human band  (~150 Hz–600 Hz): onset/footstep detector → TDOA, up to 3 tracks
// Walkers are projected onto the ground plane (array height) → x/y/range.
//
// core1 must be launched with het68_launch_core1() (see core1_launch.c).
#include "doa.h"
#include "core1_launch.h"
#include "debug_io.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Array geometry — cube standing on a vertex, mics at the six face centres.
// Override edge: HET68_DOA_EDGE_MM=150 ./build.sh
// Override height above ground (mm): HET68_DOA_HEIGHT_MM=1000 ./build.sh
// ---------------------------------------------------------------------------
#ifdef HET68_DOA_EDGE_MM
#define DOA_EDGE_MM     HET68_DOA_EDGE_MM
#else
#define DOA_EDGE_MM     512
#endif
#define DOA_EDGE_M      (DOA_EDGE_MM * 0.001f)
#define DOA_FACE_R      (DOA_EDGE_M * 0.5f)

// Vertex-down cube: centre height ≈ edge * √3/2 when the lower vertex sits on
// the ground. Used to turn human az/el into a ground (x,y) estimate.
#ifdef HET68_DOA_HEIGHT_MM
#define DOA_HEIGHT_MM   HET68_DOA_HEIGHT_MM
#else
#define DOA_HEIGHT_MM   ((DOA_EDGE_MM * 866u) / 1000u)
#endif
#define DOA_HEIGHT_M    (DOA_HEIGHT_MM * 0.001f)

static const float MIC_DIR[6][3] = {
    {  0.81650f,  0.00000f,  0.57735f },
    { -0.40825f,  0.70711f,  0.57735f },
    { -0.40825f, -0.70711f,  0.57735f },
    { -0.81650f,  0.00000f, -0.57735f },
    {  0.40825f, -0.70711f, -0.57735f },
    {  0.40825f,  0.70711f, -0.57735f },
};

static float MIC_POS[6][3];

#define DOA_FS_HZ        48000
#define DOA_C_MM_S       343000
#define DOA_FS           ((float)DOA_FS_HZ)
#define DOA_C_SOUND      (DOA_C_MM_S * 0.001f)
#define DOA_MAXLAG       ((DOA_EDGE_MM * DOA_FS_HZ + DOA_C_MM_S - 1) / DOA_C_MM_S + 2)

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
#define DOA_TDOA_SIGN    (+1.0f)
#define DOA_FILT_SETTLE  48u
#define DOA_CORR_LO      (DOA_MAXLAG + DOA_FILT_SETTLE)
#define DOA_CORR_HI      (DOA_N - DOA_MAXLAG)
#if DOA_CORR_LO >= DOA_CORR_HI
#error "DOA window too small for filter settle + lag search"
#endif

#define DOA_DRONE_RMS       2.5f
#define DOA_WIND_RATIO      0.35f
#define DOA_HUMAN_RMS       3.0f
#define DOA_HUMAN_CREST     3.5f     // peak/rms in step band → impulsive
#define DOA_DRONE_CREST_MAX 6.0f     // continuous-ish in drone band
#define DOA_ONSET_K         4.0f     // envelope vs noise floor
#define DOA_ONSET_ABS       40.0f
#define DOA_REFRACTORY      9000u    // ~187 ms @ 48 kHz between footfall DOAs
#define DOA_STREAM_CHUNK    512u

#define DOA_MAX_DRONE       2
#define DOA_MAX_HUMAN       3
#define DOA_GATE_DEG        30.0f
#define DOA_TRACK_TTL       12u      // report cycles without update → drop

#define DOA_RING_SZ         2048u
#define DOA_RING_MASK       (DOA_RING_SZ - 1u)
#if DOA_RING_SZ < 2u * DOA_N
#error "DOA_RING_SZ too small for DOA_N: increase DOA_RING_SZ"
#endif

static volatile int16_t  g_ring[DOA_RING_SZ][6];
static volatile uint32_t g_head;

volatile uint32_t g_doa_out;
volatile uint32_t g_doa_nactive;
volatile uint32_t g_doa_iter;
volatile uint32_t g_doa_ndrone;
volatile uint32_t g_doa_nhuman;

void doa_ring_push(const int16_t s6[6]) {
    uint32_t h = g_head;
    int16_t *slot = (int16_t *)g_ring[h & DOA_RING_MASK];
    for (int i = 0; i < 6; i++) slot[i] = s6[i];
    __dmb();
    g_head = h + 1u;
}

// ---------------------------------------------------------------------------
// UART helpers
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
// Biquads @ 48 kHz (Butterworth 2nd-order, DF2T). No malloc.
// ---------------------------------------------------------------------------
typedef struct { float b0, b1, b2, a1, a2; } biquad_coef_t;
typedef struct { float z1, z2; } biquad_mem_t;

static const biquad_coef_t k_hpf800 = {
    9.2862377786e-01f, -1.8572475557e+00f, 9.2862377786e-01f,
    -1.8521464854e+00f, 8.6234862603e-01f
};
static const biquad_coef_t k_lpf6000 = {
    9.7631072938e-02f, 1.9526214588e-01f, 9.7631072938e-02f,
    -9.4280904158e-01f, 3.3333333333e-01f
};
static const biquad_coef_t k_lpf250 = {
    2.6165269507e-04f, 5.2330539013e-04f, 2.6165269507e-04f,
    -1.9537279491e+00f, 9.5477455992e-01f
};
// Footstep / body-impact band ~150–600 Hz (above deep rumble, below drone hiss).
static const biquad_coef_t k_hpf150 = {
    9.8621192463e-01f, -1.9724238493e+00f, 9.8621192463e-01f,
    -1.9722337292e+00f, 9.7261396931e-01f
};
static const biquad_coef_t k_lpf600 = {
    1.4603163055e-03f, 2.9206326111e-03f, 1.4603163055e-03f,
    -1.8890330794e+00f, 8.9487434462e-01f
};

static inline float biquad_step(const biquad_coef_t *c, biquad_mem_t *s, float x) {
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

// ---------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------
typedef enum { CLS_DRONE = 1, CLS_HUMAN = 2 } src_class_t;

typedef struct {
    bool used;
    src_class_t cls;
    float az, el, conf, lvl_db;
    float x_m, y_m, range_m;
    bool have_xy;
    uint32_t age;
    uint32_t hits;
} track_t;

static track_t g_drones[DOA_MAX_DRONE];
static track_t g_humans[DOA_MAX_HUMAN];

static float ang_diff_deg(float a, float b) {
    float d = fabsf(a - b);
    return (d > 180.0f) ? (360.0f - d) : d;
}

static int track_find(track_t *tr, int nmax, float az, float el) {
    int best = -1;
    float best_d = DOA_GATE_DEG;
    for (int i = 0; i < nmax; i++) {
        if (!tr[i].used) continue;
        float d = ang_diff_deg(az, tr[i].az) + fabsf(el - tr[i].el);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static int track_alloc(track_t *tr, int nmax) {
    int free_i = -1;
    int oldest = -1;
    uint32_t oldest_age = 0;
    for (int i = 0; i < nmax; i++) {
        if (!tr[i].used) { free_i = i; break; }
        if (tr[i].age >= oldest_age) { oldest_age = tr[i].age; oldest = i; }
    }
    return (free_i >= 0) ? free_i : oldest;
}

static void track_age_all(track_t *tr, int nmax) {
    for (int i = 0; i < nmax; i++) {
        if (!tr[i].used) continue;
        tr[i].age++;
        if (tr[i].age > DOA_TRACK_TTL) tr[i].used = false;
    }
}

static uint32_t track_count(const track_t *tr, int nmax) {
    uint32_t n = 0;
    for (int i = 0; i < nmax; i++) if (tr[i].used) n++;
    return n;
}

static bool ground_xy(float az_deg, float el_deg, float *x, float *y, float *rng) {
    const float deg2rad = 3.14159265f / 180.0f;
    float el = el_deg * deg2rad;
    float az = az_deg * deg2rad;
    float dz = sinf(el);
    // Footfalls are on the ground below the array → need downward ray.
    if (dz > -0.08f) return false;
    float t = -DOA_HEIGHT_M / dz;
    float c = cosf(el);
    *x = t * c * cosf(az);
    *y = t * c * sinf(az);
    *rng = sqrtf((*x) * (*x) + (*y) * (*y));
    return (*rng > 0.15f) && (*rng < 40.0f);
}

static void track_upsert(track_t *tr, int nmax, src_class_t cls,
                         float az, float el, float conf, float lvl_db) {
    int i = track_find(tr, nmax, az, el);
    if (i < 0) i = track_alloc(tr, nmax);
    if (i < 0) return;
    // Mild EMA when updating an existing track.
    if (tr[i].used && tr[i].cls == cls) {
        tr[i].az = 0.7f * tr[i].az + 0.3f * az;
        tr[i].el = 0.7f * tr[i].el + 0.3f * el;
        tr[i].conf = 0.6f * tr[i].conf + 0.4f * conf;
        tr[i].lvl_db = 0.6f * tr[i].lvl_db + 0.4f * lvl_db;
        tr[i].hits++;
    } else {
        tr[i].az = az;
        tr[i].el = el;
        tr[i].conf = conf;
        tr[i].lvl_db = lvl_db;
        tr[i].hits = 1;
    }
    tr[i].used = true;
    tr[i].cls = cls;
    tr[i].age = 0;
    tr[i].have_xy = false;
    if (cls == CLS_HUMAN) {
        float x, y, r;
        if (ground_xy(tr[i].az, tr[i].el, &x, &y, &r)) {
            tr[i].x_m = x;
            tr[i].y_m = y;
            tr[i].range_m = r;
            tr[i].have_xy = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Shared TDOA core (operates on g_work / g_energy)
// ---------------------------------------------------------------------------
static float g_work[6][DOA_N];
static float g_energy[6];
static float s_corr[2 * DOA_MAXLAG + 1];

static float xcorr_delay(int ref, int i, float eref, float *conf) {
    const float *a = g_work[ref];
    const float *b = g_work[i];
    float best = -1e30f;
    int bestlag = 0;
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
    float norm = sqrtf(eref * g_energy[i]) + 1e-6f;
    float c = best / norm;
    *conf = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return lagf;
}

typedef struct {
    bool ok;
    float az, el, conf, lvl_db;
    float dir[3];
    int ref;
    int nactive;
} doa_fix_t;

static void load_raw_window(uint32_t h) {
    uint32_t start = h - DOA_N;
    for (uint32_t n = 0; n < DOA_N; n++) {
        uint32_t idx = (start + n) & DOA_RING_MASK;
        for (int c = 0; c < 6; c++) g_work[c][n] = (float)g_ring[idx][c];
    }
}

static doa_fix_t solve_tdoa(const bool active[6]) {
    doa_fix_t out;
    memset(&out, 0, sizeof(out));
    int nactive = 0;
    for (int c = 0; c < 6; c++) if (active[c]) nactive++;
    out.nactive = nactive;
    if (nactive < 4) return out;

    int ref = -1;
    float eref_best = -1.0f;
    for (int c = 0; c < 6; c++) {
        if (!active[c]) continue;
        if (g_energy[c] > eref_best) { eref_best = g_energy[c]; ref = c; }
    }
    out.ref = ref;
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
        for (int r = 0; r < 3; r++) {
            for (int cc = 0; cc < 3; cc++) AtA[r][cc] += conf * row[r] * row[cc];
            Atb[r] += conf * row[r] * bb;
        }
        conf_sum += conf;
        conf_n++;
    }

    float d[3];
    if (!solve3(AtA, Atb, d)) return out;
    float mag = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (mag < 1e-6f) return out;
    d[0] /= mag; d[1] /= mag; d[2] /= mag;
    out.dir[0] = d[0]; out.dir[1] = d[1]; out.dir[2] = d[2];

    float az = atan2f(d[1], d[0]) * (180.0f / 3.14159265f);
    if (az < 0.0f) az += 360.0f;
    out.az = az;
    out.el = asinf(d[2]) * (180.0f / 3.14159265f);
    out.conf = conf_n ? conf_sum / (float)conf_n : 0.0f;
    float rms = sqrtf(eref / (float)(DOA_CORR_HI - DOA_CORR_LO));
    out.lvl_db = 20.0f * log10f((rms + 1e-6f) / 32768.0f);
    out.ok = (out.conf > 0.12f);
    return out;
}

// Crude successive interference cancellation toward `dir`, then re-solve.
static void cancel_direction(const float dir[3], int ref) {
    for (int i = 0; i < 6; i++) {
        if (i == ref) continue;
        float dx = MIC_POS[i][0] - MIC_POS[ref][0];
        float dy = MIC_POS[i][1] - MIC_POS[ref][1];
        float dz = MIC_POS[i][2] - MIC_POS[ref][2];
        float lag_f = DOA_TDOA_SIGN * (dx * dir[0] + dy * dir[1] + dz * dir[2])
                      * (DOA_FS / DOA_C_SOUND);
        int lag = (int)((lag_f >= 0.0f) ? (lag_f + 0.5f) : (lag_f - 0.5f));
        if (lag < -DOA_MAXLAG) lag = -DOA_MAXLAG;
        if (lag > DOA_MAXLAG) lag = DOA_MAXLAG;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) {
            int src = (int)n + lag;
            if (src < 0 || src >= (int)DOA_N) continue;
            g_work[i][n] -= 0.85f * g_work[ref][src];
        }
    }
    // Blank the reference so the next solve picks another loud mic.
    for (uint32_t n = 0; n < DOA_N; n++) g_work[ref][n] = 0.0f;
    for (int c = 0; c < 6; c++) {
        float e = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) {
            float y = g_work[c][n];
            e += y * y;
        }
        g_energy[c] = e;
    }
}

// ---------------------------------------------------------------------------
// Band preparation helpers
// ---------------------------------------------------------------------------
static float prepare_drone_band(bool active[6]) {
    float wrat_sum = 0.0f;
    int wrat_n = 0;
    int nactive = 0;
    float crest_num = 0.0f, crest_den = 0.0f;

    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;

        biquad_mem_t hp = {0}, lp = {0}, wind = {0};
        float e_bp = 0.0f, e_wind = 0.0f, peak = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            float w = biquad_step(&k_lpf250, &wind, x);
            float y = biquad_step(&k_hpf800, &hp, x);
            y = biquad_step(&k_lpf6000, &lp, y);
            g_work[c][n] = y;
            if (n >= DOA_FILT_SETTLE) {
                e_bp += y * y;
                e_wind += w * w;
                float ay = fabsf(y);
                if (ay > peak) peak = ay;
            }
        }
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) {
            float y = g_work[c][n];
            e_corr += y * y;
        }
        g_energy[c] = e_corr;

        uint32_t n_power = DOA_N - DOA_FILT_SETTLE;
        float rms = sqrtf(e_bp / (float)n_power);
        bool loud = rms > DOA_DRONE_RMS;
        bool not_wind = e_bp >= DOA_WIND_RATIO * (e_wind + 1e-6f);
        active[c] = loud && not_wind;
        if (active[c]) nactive++;
        if (e_wind > 1e-3f) { wrat_sum += e_bp / e_wind; wrat_n++; }
        crest_num += peak;
        crest_den += rms + 1e-6f;
    }
    g_doa_nactive = (uint32_t)nactive;
    float crest = crest_num / crest_den;
    // Continuous drone-like: not a single impulse in the high band.
    if (crest > DOA_DRONE_CREST_MAX) {
        for (int c = 0; c < 6; c++) active[c] = false;
        g_doa_nactive = 0;
    }
    (void)wrat_sum; (void)wrat_n;
    return crest;
}

static float prepare_human_band(bool active[6], float *crest_out) {
    float peak_all = 0.0f, rms_all = 0.0f;
    int nactive = 0;
    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;

        biquad_mem_t hp = {0}, lp = {0};
        float e_bp = 0.0f, peak = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            float y = biquad_step(&k_hpf150, &hp, x);
            y = biquad_step(&k_lpf600, &lp, y);
            g_work[c][n] = y;
            if (n >= DOA_FILT_SETTLE) {
                e_bp += y * y;
                float ay = fabsf(y);
                if (ay > peak) peak = ay;
            }
        }
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) {
            float y = g_work[c][n];
            e_corr += y * y;
        }
        g_energy[c] = e_corr;
        uint32_t n_power = DOA_N - DOA_FILT_SETTLE;
        float rms = sqrtf(e_bp / (float)n_power);
        active[c] = rms > DOA_HUMAN_RMS;
        if (active[c]) nactive++;
        peak_all += peak;
        rms_all += rms;
    }
    float crest = peak_all / (rms_all + 1e-6f);
    *crest_out = crest;
    // Require impulsive crest for footfalls (rejects steady LF hum).
    if (crest < DOA_HUMAN_CREST) {
        for (int c = 0; c < 6; c++) active[c] = false;
        nactive = 0;
    }
    return (float)nactive;
}

// ---------------------------------------------------------------------------
// Streaming footstep onset detector (persistent filters on core1)
// ---------------------------------------------------------------------------
static biquad_mem_t g_step_hp[6], g_step_lp[6];
static float g_env;
static float g_noise;
static uint32_t g_cons;
static uint32_t g_last_onset_pos;
static bool g_onset_pending;

static void stream_step_samples(uint32_t h) {
    uint32_t pending = h - g_cons;
    if (pending > DOA_STREAM_CHUNK) {
        // Avoid unbounded catch-up after a heavy DOA; skip oldest.
        g_cons = h - DOA_STREAM_CHUNK;
        pending = DOA_STREAM_CHUNK;
    }
    for (uint32_t k = 0; k < pending; k++) {
        uint32_t idx = (g_cons++) & DOA_RING_MASK;
        float mix = 0.0f;
        for (int c = 0; c < 6; c++) {
            float x = (float)g_ring[idx][c];
            float y = biquad_step(&k_hpf150, &g_step_hp[c], x);
            y = biquad_step(&k_lpf600, &g_step_lp[c], y);
            mix += fabsf(y);
        }
        mix *= (1.0f / 6.0f);
        g_env = 0.90f * g_env + 0.10f * mix;
        // Noise floor tracks the quiet baseline slowly.
        if (g_env < g_noise) g_noise = 0.95f * g_noise + 0.05f * g_env;
        else g_noise = 0.9995f * g_noise + 0.0005f * g_env;

        bool refractory = (g_cons - g_last_onset_pos) < DOA_REFRACTORY;
        if (!refractory && g_env > DOA_ONSET_ABS && g_env > DOA_ONSET_K * (g_noise + 1.0f)) {
            g_onset_pending = true;
            g_last_onset_pos = g_cons;
        }
    }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void report_tracks(void) {
    g_doa_ndrone = track_count(g_drones, DOA_MAX_DRONE);
    g_doa_nhuman = track_count(g_humans, DOA_MAX_HUMAN);

    uint32_t lock = dbg_line_lock();
    for (int i = 0; i < DOA_MAX_DRONE; i++) {
        if (!g_drones[i].used) continue;
        dbg_puts("SRC class=drone id=");
        dbg_putu32((uint32_t)i);
        dbg_puts(" az="); put_f1(g_drones[i].az);
        dbg_puts(" el="); put_f1(g_drones[i].el);
        dbg_puts(" conf="); put_f1(g_drones[i].conf);
        dbg_puts(" lvl="); put_f1(g_drones[i].lvl_db);
        dbg_puts("dB\n");
    }
    for (int i = 0; i < DOA_MAX_HUMAN; i++) {
        if (!g_humans[i].used) continue;
        dbg_puts("SRC class=human id=");
        dbg_putu32((uint32_t)i);
        dbg_puts(" az="); put_f1(g_humans[i].az);
        dbg_puts(" el="); put_f1(g_humans[i].el);
        dbg_puts(" conf="); put_f1(g_humans[i].conf);
        dbg_puts(" lvl="); put_f1(g_humans[i].lvl_db);
        dbg_puts("dB");
        if (g_humans[i].have_xy) {
            dbg_puts(" rng="); put_f1(g_humans[i].range_m);
            dbg_puts("m x="); put_f1(g_humans[i].x_m);
            dbg_puts(" y="); put_f1(g_humans[i].y_m);
        }
        dbg_putc('\n');
    }
    dbg_puts("TRACKS drone=");
    dbg_putu32(g_doa_ndrone);
    dbg_puts(" human=");
    dbg_putu32(g_doa_nhuman);
    dbg_putc('\n');
    dbg_line_unlock(lock);
    g_doa_out++;
}

// ---------------------------------------------------------------------------
// Analysis entry points
// ---------------------------------------------------------------------------
static void analyse_drone(uint32_t h) {
    bool active[6];
    load_raw_window(h);
    (void)prepare_drone_band(active);

    doa_fix_t primary = solve_tdoa(active);
    if (primary.ok) {
        track_upsert(g_drones, DOA_MAX_DRONE, CLS_DRONE,
                     primary.az, primary.el, primary.conf, primary.lvl_db);

        // Second drone: cancel primary and re-solve if residual still loud.
        cancel_direction(primary.dir, primary.ref);
        bool active2[6];
        int n2 = 0;
        for (int c = 0; c < 6; c++) {
            float rms = sqrtf(g_energy[c] / (float)(DOA_CORR_HI - DOA_CORR_LO));
            active2[c] = rms > DOA_DRONE_RMS * 0.7f;
            if (active2[c]) n2++;
        }
        if (n2 >= 4) {
            doa_fix_t secondary = solve_tdoa(active2);
            if (secondary.ok &&
                (ang_diff_deg(secondary.az, primary.az) +
                 fabsf(secondary.el - primary.el)) > DOA_GATE_DEG) {
                track_upsert(g_drones, DOA_MAX_DRONE, CLS_DRONE,
                             secondary.az, secondary.el,
                             secondary.conf * 0.85f, secondary.lvl_db);
            }
        }
    }
}

static void analyse_human_onset(uint32_t h) {
    bool active[6];
    float crest = 0.0f;
    load_raw_window(h);
    (void)prepare_human_band(active, &crest);
    doa_fix_t r = solve_tdoa(active);
    if (!r.ok) return;
    // Prefer downward elevation for ground walkers; still accept shallow angles.
    track_upsert(g_humans, DOA_MAX_HUMAN, CLS_HUMAN, r.az, r.el, r.conf, r.lvl_db);
}

static bool doa_core1_verify(void) {
    if (!g_core1_alive) return false;
    uint32_t t0 = g_doa_iter;
    sleep_ms(80);
    uint32_t t1 = g_doa_iter;
    if ((t1 - t0) < 50000u) return false;
    sleep_ms(80);
    return (g_doa_iter - t1) >= 50000u;
}

static void doa_core1_main(void) {
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 3; k++) MIC_POS[i][k] = MIC_DIR[i][k] * DOA_FACE_R;

    g_cons = g_head;
    g_noise = 20.0f;
    g_env = 0.0f;
    g_last_onset_pos = g_cons;
    uint32_t last_drone = g_head;

    for (;;) {
        g_doa_iter++;
        uint32_t h = g_head;

        stream_step_samples(h);

        if (g_onset_pending) {
            g_onset_pending = false;
            analyse_human_onset(h);
        }

        if ((uint32_t)(h - last_drone) >= DOA_OUT_SAMPLES) {
            last_drone = h;
            track_age_all(g_drones, DOA_MAX_DRONE);
            track_age_all(g_humans, DOA_MAX_HUMAN);
            analyse_drone(h);
            report_tracks();
        } else {
            tight_loop_contents();
        }
    }
}

void doa_start(void) {
    if (!het68_launch_core1_verify(doa_core1_main, doa_core1_verify)) {
        uint32_t s = dbg_line_lock();
        dbg_puts("DOA: core1 launch FAILED\n");
        dbg_line_unlock(s);
    }
}
