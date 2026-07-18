// doa.c — multi-source 3D DOA on core1 with drone + walker + vehicle diarization.
//
// core0 pushes each 6-channel frame via doa_ring_push() from the USB/I2S feed.
// core1 runs:
//   • drone band (~800 Hz–6 kHz): continuous TDOA, up to 2 tracks (primary + SIC)
//   • walker band (~150–600 Hz): single walking entity — onset bout → human/cat/dog
//   • vehicle band (~80 Hz–2.5 kHz): pass-by → ICE vs EV
// Entity signatures persist in flash via entity_store (re-ID across reboots).
//
// core1 must be launched with het68_launch_core1() (see core1_launch.c).
#include "doa.h"
#include "entity_store.h"
#include "core1_launch.h"
#include "debug_io.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Array geometry — cube standing on a vertex, mics at the six face centres.
// Override edge:   HET68_DOA_EDGE_MM=150 ./build.sh
// Override height: HET68_DOA_HEIGHT_MM=1000 ./build.sh
// ---------------------------------------------------------------------------
#ifdef HET68_DOA_EDGE_MM
#define DOA_EDGE_MM     HET68_DOA_EDGE_MM
#else
#define DOA_EDGE_MM     512
#endif
#define DOA_EDGE_M      (DOA_EDGE_MM * 0.001f)
#define DOA_FACE_R      (DOA_EDGE_M * 0.5f)

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
#define DOA_WALK_RMS        3.0f
#define DOA_WALK_CREST      3.5f
#define DOA_DRONE_CREST_MAX 6.0f
#define DOA_ONSET_K         4.0f
#define DOA_ONSET_ABS       40.0f
#define DOA_REFRACTORY      4800u    // ~100 ms — allows cat/dog cadence
#define DOA_STREAM_CHUNK    512u
#define DOA_BOUT_GAP        120000u  // ~2.5 s silence ends a walking bout
#define DOA_BOUT_MIN_STEPS  3u
#define DOA_BOUT_MAX_STEPS  16u

#define DOA_MAX_DRONE       2
#define DOA_MAX_VEHICLE     2
#define DOA_GATE_DEG        30.0f
#define DOA_TRACK_TTL       12u
#define DOA_VEH_MIN_FRAMES  4u       // ~0.8 s continuous before vehicle entity
#define DOA_VEH_GAP_FRAMES  3u
#define DOA_VEH_RMS         6.0f
#define DOA_VEH_CREST_MAX   4.5f     // continuous pass-by, not footfall spikes

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
volatile uint32_t g_doa_nwalker;
volatile uint32_t g_doa_nvehicle;
volatile uint32_t g_doa_entity_id;

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
static const biquad_coef_t k_hpf150 = {
    9.8621192463e-01f, -1.9724238493e+00f, 9.8621192463e-01f,
    -1.9722337292e+00f, 9.7261396931e-01f
};
static const biquad_coef_t k_lpf600 = {
    1.4603163055e-03f, 2.9206326111e-03f, 1.4603163055e-03f,
    -1.8890330794e+00f, 8.9487434462e-01f
};
static const biquad_coef_t k_hpf400 = {
    9.6365276396e-01f, -1.9273055279e+00f, 9.6365276396e-01f,
    -1.9259839697e+00f, 9.2862708612e-01f
};
// Vehicle pass-by band ~80 Hz–2.5 kHz + mid split ~300–1200 Hz.
static const biquad_coef_t k_hpf80 = {
    9.9262254276e-01f, -1.9852450855e+00f, 9.9262254276e-01f,
    -1.9851906579e+00f, 9.8529951313e-01f
};
static const biquad_coef_t k_lpf2500 = {
    2.1620718376e-02f, 4.3241436753e-02f, 2.1620718376e-02f,
    -1.5431211312e+00f, 6.2960400474e-01f
};
static const biquad_coef_t k_hpf300 = {
    9.7261389850e-01f, -1.9452277970e+00f, 9.7261389850e-01f,
    -1.9444776578e+00f, 9.4597793623e-01f
};
static const biquad_coef_t k_lpf1200 = {
    5.5427172103e-03f, 1.1085434421e-02f, 5.5427172103e-03f,
    -1.7786317778e+00f, 8.0080264667e-01f
};

static inline float biquad_step(const biquad_coef_t *c, biquad_mem_t *s, float x) {
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

// ---------------------------------------------------------------------------
// Classes / tracks (gallery lives in entity_store — flash-backed)
// ---------------------------------------------------------------------------
typedef enum {
    CLS_NONE  = 0,
    CLS_DRONE = 1,
    CLS_HUMAN = ENT_HUMAN,
    CLS_CAT   = ENT_CAT,
    CLS_DOG   = ENT_DOG,
    CLS_ICE   = ENT_ICE,
    CLS_EV    = ENT_EV
} src_class_t;

static const char *class_name(src_class_t c) {
    if (c == CLS_DRONE) return "drone";
    if (c == CLS_NONE) return "none";
    return entity_class_name((entity_class_t)c);
}

typedef struct {
    bool used;
    src_class_t cls;
    float az, el, conf, lvl_db;
    float x_m, y_m, range_m;
    bool have_xy;
    uint32_t age;
    uint32_t entity_id;   // gallery id (walkers/vehicles); 0 for drones
    float match;          // 0..1 similarity to gallery template
    float cadence_hz;
} track_t;

static track_t g_drones[DOA_MAX_DRONE];
static track_t g_vehicles[DOA_MAX_VEHICLE];
static track_t g_walker;                 // single walking entity

static float ang_diff_deg(float a, float b) {
    float d = fabsf(a - b);
    return (d > 180.0f) ? (360.0f - d) : d;
}

static int track_find_drone(float az, float el) {
    int best = -1;
    float best_d = DOA_GATE_DEG;
    for (int i = 0; i < DOA_MAX_DRONE; i++) {
        if (!g_drones[i].used) continue;
        float d = ang_diff_deg(az, g_drones[i].az) + fabsf(el - g_drones[i].el);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static int track_alloc_drone(void) {
    for (int i = 0; i < DOA_MAX_DRONE; i++) if (!g_drones[i].used) return i;
    int oldest = 0;
    for (int i = 1; i < DOA_MAX_DRONE; i++)
        if (g_drones[i].age > g_drones[oldest].age) oldest = i;
    return oldest;
}

static void track_age_drones(void) {
    for (int i = 0; i < DOA_MAX_DRONE; i++) {
        if (!g_drones[i].used) continue;
        g_drones[i].age++;
        if (g_drones[i].age > DOA_TRACK_TTL) g_drones[i].used = false;
    }
}

static uint32_t track_count_drones(void) {
    uint32_t n = 0;
    for (int i = 0; i < DOA_MAX_DRONE; i++) if (g_drones[i].used) n++;
    return n;
}

static int track_find_vehicle(float az, float el) {
    int best = -1;
    float best_d = DOA_GATE_DEG;
    for (int i = 0; i < DOA_MAX_VEHICLE; i++) {
        if (!g_vehicles[i].used) continue;
        float d = ang_diff_deg(az, g_vehicles[i].az) + fabsf(el - g_vehicles[i].el);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static int track_alloc_vehicle(void) {
    for (int i = 0; i < DOA_MAX_VEHICLE; i++) if (!g_vehicles[i].used) return i;
    int oldest = 0;
    for (int i = 1; i < DOA_MAX_VEHICLE; i++)
        if (g_vehicles[i].age > g_vehicles[oldest].age) oldest = i;
    return oldest;
}

static void track_age_vehicles(void) {
    for (int i = 0; i < DOA_MAX_VEHICLE; i++) {
        if (!g_vehicles[i].used) continue;
        g_vehicles[i].age++;
        if (g_vehicles[i].age > DOA_TRACK_TTL) g_vehicles[i].used = false;
    }
}

static uint32_t track_count_vehicles(void) {
    uint32_t n = 0;
    for (int i = 0; i < DOA_MAX_VEHICLE; i++) if (g_vehicles[i].used) n++;
    return n;
}

static void vehicle_upsert(src_class_t cls, float az, float el, float conf,
                           float lvl_db, uint32_t eid, float match, float mod_hz) {
    int i = track_find_vehicle(az, el);
    if (i < 0) i = track_alloc_vehicle();
    if (g_vehicles[i].used) {
        g_vehicles[i].az = 0.7f * g_vehicles[i].az + 0.3f * az;
        g_vehicles[i].el = 0.7f * g_vehicles[i].el + 0.3f * el;
        g_vehicles[i].conf = 0.6f * g_vehicles[i].conf + 0.4f * conf;
        g_vehicles[i].lvl_db = 0.6f * g_vehicles[i].lvl_db + 0.4f * lvl_db;
    } else {
        g_vehicles[i].az = az;
        g_vehicles[i].el = el;
        g_vehicles[i].conf = conf;
        g_vehicles[i].lvl_db = lvl_db;
    }
    g_vehicles[i].used = true;
    g_vehicles[i].cls = cls;
    g_vehicles[i].age = 0;
    g_vehicles[i].entity_id = eid;
    g_vehicles[i].match = match;
    g_vehicles[i].cadence_hz = mod_hz;
    g_vehicles[i].have_xy = false;
}

static bool ground_xy(float az_deg, float el_deg, float *x, float *y, float *rng) {
    const float deg2rad = 3.14159265f / 180.0f;
    float el = el_deg * deg2rad;
    float az = az_deg * deg2rad;
    float dz = sinf(el);
    if (dz > -0.08f) return false;
    float t = -DOA_HEIGHT_M / dz;
    float c = cosf(el);
    *x = t * c * cosf(az);
    *y = t * c * sinf(az);
    *rng = sqrtf((*x) * (*x) + (*y) * (*y));
    return (*rng > 0.15f) && (*rng < 40.0f);
}

static void drone_upsert(float az, float el, float conf, float lvl_db) {
    int i = track_find_drone(az, el);
    if (i < 0) i = track_alloc_drone();
    if (g_drones[i].used) {
        g_drones[i].az = 0.7f * g_drones[i].az + 0.3f * az;
        g_drones[i].el = 0.7f * g_drones[i].el + 0.3f * el;
        g_drones[i].conf = 0.6f * g_drones[i].conf + 0.4f * conf;
        g_drones[i].lvl_db = 0.6f * g_drones[i].lvl_db + 0.4f * lvl_db;
    } else {
        g_drones[i].az = az;
        g_drones[i].el = el;
        g_drones[i].conf = conf;
        g_drones[i].lvl_db = lvl_db;
    }
    g_drones[i].used = true;
    g_drones[i].cls = CLS_DRONE;
    g_drones[i].age = 0;
    g_drones[i].entity_id = 0;
    g_drones[i].have_xy = false;
}

// ---------------------------------------------------------------------------
// Shared TDOA core
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

static float prepare_drone_band(bool active[6]) {
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
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) e_corr += g_work[c][n] * g_work[c][n];
        g_energy[c] = e_corr;
        float rms = sqrtf(e_bp / (float)(DOA_N - DOA_FILT_SETTLE));
        active[c] = (rms > DOA_DRONE_RMS) && (e_bp >= DOA_WIND_RATIO * (e_wind + 1e-6f));
        if (active[c]) nactive++;
        crest_num += peak;
        crest_den += rms + 1e-6f;
    }
    g_doa_nactive = (uint32_t)nactive;
    float crest = crest_num / crest_den;
    if (crest > DOA_DRONE_CREST_MAX) {
        for (int c = 0; c < 6; c++) active[c] = false;
        g_doa_nactive = 0;
    }
    return crest;
}

// Walker band + spectral split energies (low 150–250, high 400–600).
typedef struct {
    float crest;
    float e_full;
    float e_low;
    float e_high;
    float peak;
} walk_feat_t;

static walk_feat_t prepare_walker_band(bool active[6]) {
    walk_feat_t f;
    memset(&f, 0, sizeof(f));
    float peak_all = 0.0f, rms_all = 0.0f;
    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;

        biquad_mem_t hp = {0}, lp = {0}, lo = {0}, hi = {0};
        float e_bp = 0.0f, e_lo = 0.0f, e_hi = 0.0f, peak = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            float y = biquad_step(&k_hpf150, &hp, x);
            y = biquad_step(&k_lpf600, &lp, y);
            float yl = biquad_step(&k_lpf250, &lo, y);
            float yh = biquad_step(&k_hpf400, &hi, y);
            g_work[c][n] = y;
            if (n >= DOA_FILT_SETTLE) {
                e_bp += y * y;
                e_lo += yl * yl;
                e_hi += yh * yh;
                float ay = fabsf(y);
                if (ay > peak) peak = ay;
            }
        }
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) e_corr += g_work[c][n] * g_work[c][n];
        g_energy[c] = e_corr;
        float rms = sqrtf(e_bp / (float)(DOA_N - DOA_FILT_SETTLE));
        active[c] = rms > DOA_WALK_RMS;
        peak_all += peak;
        rms_all += rms;
        f.e_full += e_bp;
        f.e_low += e_lo;
        f.e_high += e_hi;
        if (peak > f.peak) f.peak = peak;
    }
    f.crest = peak_all / (rms_all + 1e-6f);
    if (f.crest < DOA_WALK_CREST) {
        for (int c = 0; c < 6; c++) active[c] = false;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Walking bout → species class + entity gallery
// ---------------------------------------------------------------------------
typedef struct {
    bool active;
    uint32_t n_steps;
    uint32_t last_onset;
    uint32_t onset_pos[DOA_BOUT_MAX_STEPS];
    float az_sum, el_sum, conf_sum, lvl_sum;
    float e_full, e_low, e_high, peak_sum, crest_sum;
} bout_t;

static bout_t g_bout;

static src_class_t classify_species(float cadence_hz, float low_r, float high_r,
                                    float peak_db, float crest) {
    float sh = 0.0f, sd = 0.0f, sc = 0.0f;

    // Cadence priors (walking): human slower, cat faster, dog in between.
    if (cadence_hz >= 1.1f && cadence_hz <= 2.4f) sh += 2.5f;
    if (cadence_hz >= 1.8f && cadence_hz <= 4.2f) sd += 2.0f;
    if (cadence_hz >= 2.6f && cadence_hz <= 5.5f) sc += 2.5f;

    // Spectral: humans thumpier (low), cats lighter/clickier (high).
    if (low_r > 0.42f) sh += 2.0f;
    if (low_r > 0.30f && low_r <= 0.45f) sd += 1.0f;
    if (high_r > 0.32f) sc += 2.0f;
    if (high_r > 0.22f && high_r <= 0.35f) sd += 1.5f;
    if (high_r < 0.18f && low_r > 0.40f) sh += 1.0f;

    // Level / crest: humans louder impacts; cats softer.
    if (peak_db > -38.0f) sh += 1.5f;
    if (peak_db <= -38.0f && peak_db > -48.0f) sd += 1.0f;
    if (peak_db <= -45.0f) sc += 1.5f;
    if (crest > 5.0f) { sh += 0.5f; sd += 0.5f; }
    if (crest > 6.5f && peak_db <= -42.0f) sc += 0.5f;

    if (sh >= sd && sh >= sc) return CLS_HUMAN;
    if (sd >= sc) return CLS_DOG;
    return CLS_CAT;
}

static void sig_from_bout(const bout_t *b, entity_sig_t *s) {
    float ipi_sum = 0.0f;
    uint32_t nipi = 0;
    for (uint32_t i = 1; i < b->n_steps; i++) {
        uint32_t d = b->onset_pos[i] - b->onset_pos[i - 1];
        if (d > 3000u && d < 90000u) { // ~60 ms … ~1.9 s
            ipi_sum += (float)d;
            nipi++;
        }
    }
    float ipi = nipi ? (ipi_sum / (float)nipi) : (float)DOA_FS_HZ;
    s->cadence_hz = DOA_FS / (ipi + 1.0f);
    float n = (float)b->n_steps;
    float peak = b->peak_sum / n;
    s->peak_db = 20.0f * log10f((peak + 1e-6f) / 32768.0f);
    s->low_ratio = b->e_low / (b->e_full + 1e-6f);
    s->high_ratio = b->e_high / (b->e_full + 1e-6f);
    s->mid_ratio = 0.0f;
    s->crest = b->crest_sum / n;
    float az = b->az_sum / n;
    float el = b->el_sum / n;
    s->az_n = az / 360.0f;
    s->el_n = (el + 90.0f) / 180.0f;
}

static void finalize_bout_if_needed(uint32_t now, bool force) {
    if (!g_bout.active) return;
    bool gap = (now - g_bout.last_onset) >= DOA_BOUT_GAP;
    if (!force && !gap) return;
    if (g_bout.n_steps < DOA_BOUT_MIN_STEPS) {
        g_bout.active = false;
        g_bout.n_steps = 0;
        return;
    }

    entity_sig_t sig;
    sig_from_bout(&g_bout, &sig);
    src_class_t cls = classify_species(sig.cadence_hz, sig.low_ratio, sig.high_ratio,
                                       sig.peak_db, sig.crest);

    float match = 0.0f;
    uint32_t eid = entity_store_match_or_create((entity_class_t)cls, &sig, &match);

    float n = (float)g_bout.n_steps;
    float az = g_bout.az_sum / n;
    float el = g_bout.el_sum / n;
    float conf = g_bout.conf_sum / n;
    float lvl = g_bout.lvl_sum / n;

    g_walker.used = true;
    g_walker.cls = cls;
    g_walker.az = az;
    g_walker.el = el;
    g_walker.conf = conf;
    g_walker.lvl_db = lvl;
    g_walker.age = 0;
    g_walker.entity_id = eid;
    g_walker.match = match;
    g_walker.cadence_hz = sig.cadence_hz;
    g_walker.have_xy = false;
    float x, y, r;
    if (ground_xy(az, el, &x, &y, &r)) {
        g_walker.x_m = x;
        g_walker.y_m = y;
        g_walker.range_m = r;
        g_walker.have_xy = true;
    }
    g_doa_entity_id = eid;
    g_doa_nwalker = 1;

    uint32_t lock = dbg_line_lock();
    dbg_puts("ENTITY id=");
    dbg_putu32(eid);
    dbg_puts(" class=");
    dbg_puts(class_name(cls));
    dbg_puts(" steps=");
    dbg_putu32(g_bout.n_steps);
    dbg_puts(" cadence=");
    put_f2(sig.cadence_hz);
    dbg_puts("Hz match=");
    put_f2(match);
    dbg_puts(" low=");
    put_f2(sig.low_ratio);
    dbg_puts(" high=");
    put_f2(sig.high_ratio);
    dbg_putc('\n');
    dbg_line_unlock(lock);

    g_bout.active = false;
    g_bout.n_steps = 0;
}

static void bout_add_step(uint32_t onset_pos, const doa_fix_t *r, const walk_feat_t *f) {
    if (!g_bout.active || (onset_pos - g_bout.last_onset) >= DOA_BOUT_GAP) {
        g_bout.active = true;
        g_bout.n_steps = 0;
        g_bout.az_sum = g_bout.el_sum = g_bout.conf_sum = g_bout.lvl_sum = 0.0f;
        g_bout.e_full = g_bout.e_low = g_bout.e_high = 0.0f;
        g_bout.peak_sum = g_bout.crest_sum = 0.0f;
    }
    if (g_bout.n_steps < DOA_BOUT_MAX_STEPS) {
        g_bout.onset_pos[g_bout.n_steps] = onset_pos;
        g_bout.n_steps++;
    } else {
        // Shift left to keep recent steps.
        for (uint32_t i = 1; i < DOA_BOUT_MAX_STEPS; i++)
            g_bout.onset_pos[i - 1] = g_bout.onset_pos[i];
        g_bout.onset_pos[DOA_BOUT_MAX_STEPS - 1] = onset_pos;
    }
    g_bout.last_onset = onset_pos;
    g_bout.az_sum += r->az;
    g_bout.el_sum += r->el;
    g_bout.conf_sum += r->conf;
    g_bout.lvl_sum += r->lvl_db;
    g_bout.e_full += f->e_full;
    g_bout.e_low += f->e_low;
    g_bout.e_high += f->e_high;
    g_bout.peak_sum += f->peak;
    g_bout.crest_sum += f->crest;

    // Live preview on the single walker track (class unknown until bout ends).
    g_walker.used = true;
    g_walker.cls = CLS_NONE;
    g_walker.az = r->az;
    g_walker.el = r->el;
    g_walker.conf = r->conf;
    g_walker.lvl_db = r->lvl_db;
    g_walker.age = 0;
    g_walker.entity_id = g_doa_entity_id; // last known until finalize
    g_walker.match = 0.0f;
    g_walker.cadence_hz = 0.0f;
    g_walker.have_xy = false;
    float x, y, rng;
    if (ground_xy(r->az, r->el, &x, &y, &rng)) {
        g_walker.x_m = x;
        g_walker.y_m = y;
        g_walker.range_m = rng;
        g_walker.have_xy = true;
    }
    g_doa_nwalker = 1;
}

// ---------------------------------------------------------------------------
// Streaming footstep onset detector
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
// Vehicle pass-by (ICE vs EV) — continuous band, not footfall onsets
// ---------------------------------------------------------------------------
typedef struct {
    bool active;
    uint32_t frames;
    uint32_t quiet;
    float az_sum, el_sum, conf_sum, lvl_sum;
    float e_full, e_low, e_mid, e_high;
    float crest_sum;
    float env_var_sum;
    float az_first, az_last;
} vehicle_bout_t;

static vehicle_bout_t g_veh;

typedef struct {
    float e_full, e_low, e_mid, e_high;
    float crest;
    float env_var;   // envelope variance → roughness / modulation proxy
    float peak;
} veh_feat_t;

static veh_feat_t prepare_vehicle_band(bool active[6]) {
    veh_feat_t f;
    memset(&f, 0, sizeof(f));
    float peak_all = 0.0f, rms_all = 0.0f;
    float env_prev = 0.0f, env_d2 = 0.0f;
    uint32_t env_n = 0;

    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;

        biquad_mem_t hp = {0}, lp = {0}, lo = {0}, mh = {0}, ml = {0};
        float e_bp = 0.0f, e_lo = 0.0f, e_mid = 0.0f, e_hi = 0.0f, peak = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            float y = biquad_step(&k_hpf80, &hp, x);
            y = biquad_step(&k_lpf2500, &lp, y);
            float yl = biquad_step(&k_lpf250, &lo, y);
            float ym = biquad_step(&k_hpf300, &mh, y);
            ym = biquad_step(&k_lpf1200, &ml, ym);
            // high within vehicle band ≈ residual after mid/low emphasis
            float yh = y - yl;
            g_work[c][n] = y;
            if (n >= DOA_FILT_SETTLE) {
                e_bp += y * y;
                e_lo += yl * yl;
                e_mid += ym * ym;
                e_hi += yh * yh;
                float ay = fabsf(y);
                if (ay > peak) peak = ay;
                if (c == 0) {
                    float d = ay - env_prev;
                    env_d2 += d * d;
                    env_prev = ay;
                    env_n++;
                }
            }
        }
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) e_corr += g_work[c][n] * g_work[c][n];
        g_energy[c] = e_corr;
        float rms = sqrtf(e_bp / (float)(DOA_N - DOA_FILT_SETTLE));
        active[c] = rms > DOA_VEH_RMS;
        peak_all += peak;
        rms_all += rms;
        f.e_full += e_bp;
        f.e_low += e_lo;
        f.e_mid += e_mid;
        f.e_high += e_hi;
        if (peak > f.peak) f.peak = peak;
    }
    f.crest = peak_all / (rms_all + 1e-6f);
    f.env_var = env_n ? (env_d2 / (float)env_n) : 0.0f;
    // Reject impulsive (footsteps) and too-quiet windows.
    if (f.crest > DOA_VEH_CREST_MAX) {
        for (int c = 0; c < 6; c++) active[c] = false;
    }
    return f;
}

static src_class_t classify_vehicle(float low_r, float mid_r, float high_r,
                                    float peak_db, float env_var) {
    float s_ice = 0.0f, s_ev = 0.0f;
    // ICE: stronger LF rumble / engine; often rougher envelope.
    if (low_r > 0.40f) s_ice += 2.5f;
    if (low_r > 0.30f && low_r <= 0.45f) s_ice += 1.0f;
    if (env_var > 200.0f) s_ice += 1.0f;
    if (peak_db > -35.0f) s_ice += 1.0f;

    // EV: quieter, tire/whine mid-high, weaker LF.
    if (low_r < 0.35f) s_ev += 2.0f;
    if (mid_r > 0.25f) s_ev += 1.5f;
    if (high_r > 0.25f) s_ev += 1.5f;
    if (peak_db <= -35.0f) s_ev += 1.0f;
    if (env_var <= 200.0f) s_ev += 0.5f;

    return (s_ice >= s_ev) ? CLS_ICE : CLS_EV;
}

static void finalize_vehicle_bout(void) {
    if (!g_veh.active || g_veh.frames < DOA_VEH_MIN_FRAMES) {
        g_veh.active = false;
        g_veh.frames = 0;
        return;
    }
    float n = (float)g_veh.frames;
    float az = g_veh.az_sum / n;
    float el = g_veh.el_sum / n;
    float conf = g_veh.conf_sum / n;
    float lvl = g_veh.lvl_sum / n;
    float low_r = g_veh.e_low / (g_veh.e_full + 1e-6f);
    float mid_r = g_veh.e_mid / (g_veh.e_full + 1e-6f);
    float high_r = g_veh.e_high / (g_veh.e_full + 1e-6f);
    float crest = g_veh.crest_sum / n;
    float env_var = g_veh.env_var_sum / n;
    float daz = ang_diff_deg(g_veh.az_first, g_veh.az_last);
    // Modulation proxy from envelope variance (not a true Hz; stored in cadence slot).
    float mod_hz = sqrtf(env_var + 1.0f) * 0.01f;
    if (daz > 15.0f) mod_hz += 0.5f; // moving pass-by bonus

    src_class_t cls = classify_vehicle(low_r, mid_r, high_r, lvl, env_var);

    entity_sig_t sig;
    sig.cadence_hz = mod_hz;
    sig.peak_db = lvl;
    sig.low_ratio = low_r;
    sig.mid_ratio = mid_r;
    sig.high_ratio = high_r;
    sig.crest = crest;
    sig.az_n = az / 360.0f;
    sig.el_n = (el + 90.0f) / 180.0f;

    float match = 0.0f;
    uint32_t eid = entity_store_match_or_create((entity_class_t)cls, &sig, &match);
    vehicle_upsert(cls, az, el, conf, lvl, eid, match, mod_hz);
    g_doa_entity_id = eid;
    g_doa_nvehicle = track_count_vehicles();

    uint32_t lock = dbg_line_lock();
    dbg_puts("ENTITY id=");
    dbg_putu32(eid);
    dbg_puts(" class=");
    dbg_puts(class_name(cls));
    dbg_puts(" frames=");
    dbg_putu32(g_veh.frames);
    dbg_puts(" match=");
    put_f2(match);
    dbg_puts(" low=");
    put_f2(low_r);
    dbg_puts(" mid=");
    put_f2(mid_r);
    dbg_puts(" high=");
    put_f2(high_r);
    dbg_puts(" daz=");
    put_f1(daz);
    dbg_puts("deg\n");
    dbg_line_unlock(lock);

    g_veh.active = false;
    g_veh.frames = 0;
    g_veh.quiet = 0;
}

static void analyse_vehicle(uint32_t h) {
    bool active[6];
    load_raw_window(h);
    veh_feat_t feat = prepare_vehicle_band(active);
    doa_fix_t r = solve_tdoa(active);

    if (!r.ok) {
        if (g_veh.active) {
            g_veh.quiet++;
            if (g_veh.quiet >= DOA_VEH_GAP_FRAMES) finalize_vehicle_bout();
        }
        return;
    }

    // Prefer near-horizon sources for road vehicles.
    if (r.el > 35.0f || r.el < -55.0f) {
        if (g_veh.active) {
            g_veh.quiet++;
            if (g_veh.quiet >= DOA_VEH_GAP_FRAMES) finalize_vehicle_bout();
        }
        return;
    }

    if (!g_veh.active) {
        memset(&g_veh, 0, sizeof(g_veh));
        g_veh.active = true;
        g_veh.az_first = r.az;
    }
    g_veh.quiet = 0;
    g_veh.frames++;
    g_veh.az_sum += r.az;
    g_veh.el_sum += r.el;
    g_veh.conf_sum += r.conf;
    g_veh.lvl_sum += r.lvl_db;
    g_veh.e_full += feat.e_full;
    g_veh.e_low += feat.e_low;
    g_veh.e_mid += feat.e_mid;
    g_veh.e_high += feat.e_high;
    g_veh.crest_sum += feat.crest;
    g_veh.env_var_sum += feat.env_var;
    g_veh.az_last = r.az;

    // Live preview track (class unknown until finalize).
    vehicle_upsert(CLS_NONE, r.az, r.el, r.conf, r.lvl_db, 0, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void report_tracks(void) {
    g_doa_ndrone = track_count_drones();
    g_doa_nwalker = g_walker.used ? 1u : 0u;
    g_doa_nvehicle = track_count_vehicles();
    if (g_walker.used && g_walker.entity_id) g_doa_entity_id = g_walker.entity_id;

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
    for (int i = 0; i < DOA_MAX_VEHICLE; i++) {
        if (!g_vehicles[i].used) continue;
        dbg_puts("SRC class=");
        dbg_puts(g_vehicles[i].cls == CLS_NONE ? "vehicle" : class_name(g_vehicles[i].cls));
        dbg_puts(" entity=");
        dbg_putu32(g_vehicles[i].entity_id);
        dbg_puts(" az="); put_f1(g_vehicles[i].az);
        dbg_puts(" el="); put_f1(g_vehicles[i].el);
        dbg_puts(" conf="); put_f1(g_vehicles[i].conf);
        dbg_puts(" lvl="); put_f1(g_vehicles[i].lvl_db);
        dbg_puts("dB");
        if (g_vehicles[i].cls != CLS_NONE) {
            dbg_puts(" match=");
            put_f2(g_vehicles[i].match);
        }
        dbg_putc('\n');
    }
    if (g_walker.used) {
        dbg_puts("SRC class=");
        dbg_puts(g_walker.cls == CLS_NONE ? "walker" : class_name(g_walker.cls));
        dbg_puts(" entity=");
        dbg_putu32(g_walker.entity_id);
        dbg_puts(" az="); put_f1(g_walker.az);
        dbg_puts(" el="); put_f1(g_walker.el);
        dbg_puts(" conf="); put_f1(g_walker.conf);
        dbg_puts(" lvl="); put_f1(g_walker.lvl_db);
        dbg_puts("dB");
        if (g_walker.cls != CLS_NONE) {
            dbg_puts(" match=");
            put_f2(g_walker.match);
            dbg_puts(" cadence=");
            put_f2(g_walker.cadence_hz);
            dbg_puts("Hz");
        }
        if (g_walker.have_xy) {
            dbg_puts(" rng="); put_f1(g_walker.range_m);
            dbg_puts("m x="); put_f1(g_walker.x_m);
            dbg_puts(" y="); put_f1(g_walker.y_m);
        }
        dbg_putc('\n');
    }
    dbg_puts("TRACKS drone=");
    dbg_putu32(g_doa_ndrone);
    dbg_puts(" vehicle=");
    dbg_putu32(g_doa_nvehicle);
    dbg_puts(" walker=");
    dbg_putu32(g_doa_nwalker);
    dbg_puts(" entity=");
    dbg_putu32(g_doa_entity_id);
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
    if (!primary.ok) return;

    drone_upsert(primary.az, primary.el, primary.conf, primary.lvl_db);

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
            drone_upsert(secondary.az, secondary.el,
                         secondary.conf * 0.85f, secondary.lvl_db);
        }
    }
}

static void analyse_walker_onset(uint32_t h) {
    bool active[6];
    load_raw_window(h);
    walk_feat_t feat = prepare_walker_band(active);
    doa_fix_t r = solve_tdoa(active);
    if (!r.ok) return;
    bout_add_step(g_last_onset_pos, &r, &feat);
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
    // Allow the other core to lock us out during flash_safe_execute saves.
    entity_store_core_init();

    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 3; k++) MIC_POS[i][k] = MIC_DIR[i][k] * DOA_FACE_R;

    g_cons = g_head;
    g_noise = 20.0f;
    g_env = 0.0f;
    g_last_onset_pos = g_cons;
    uint32_t last_drone = g_head;
    memset(&g_bout, 0, sizeof(g_bout));
    memset(&g_walker, 0, sizeof(g_walker));
    memset(&g_veh, 0, sizeof(g_veh));
    memset(g_vehicles, 0, sizeof(g_vehicles));

    for (;;) {
        g_doa_iter++;
        uint32_t h = g_head;

        stream_step_samples(h);
        finalize_bout_if_needed(g_cons, false);

        if (g_onset_pending) {
            g_onset_pending = false;
            analyse_walker_onset(h);
        }

        if ((uint32_t)(h - last_drone) >= DOA_OUT_SAMPLES) {
            last_drone = h;
            track_age_drones();
            track_age_vehicles();
            if (g_walker.used) {
                g_walker.age++;
                if (g_walker.age > DOA_TRACK_TTL) {
                    g_walker.used = false;
                    g_doa_nwalker = 0;
                }
            }
            analyse_drone(h);
            analyse_vehicle(h);
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
