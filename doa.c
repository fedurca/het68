// doa.c — multi-source 3D DOA on core1 with drone + walker + vehicle + bird + wind.
//
// core0 pushes each 6-channel frame via doa_ring_push() from the USB/I2S feed.
// core1 runs:
//   • drone band (~800 Hz–6 kHz): continuous TDOA, up to 2 tracks (primary + SIC)
//   • wind (LPF ~250 Hz): keep wind gate for drones; also report intensity + direction
//   • walker band (~150–600 Hz): single walking entity — onset bout → human/cat/dog
//   • vehicle band (~80 Hz–2.5 kHz): pass-by → ICE vs EV
//   • bird band (~2–8 kHz): songbird / corvid / bird
// Entity signatures → entity_store; timed events → detection_log (after TIME SYNC).
//
// core1 must be launched with het68_launch_core1() (see core1_launch.c).
#include "doa.h"
#include "entity_store.h"
#include "detection_log.h"
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
#define DOA_WIND_RATIO      0.38f    // slightly stricter LF wind reject for drones
#define DOA_WIND_RMS_MIN    12.0f   // report wind when LF energy exceeds this
#define DOA_WALK_RMS        3.0f
#define DOA_WALK_CREST      3.7f    // footsteps are impulsive
#define DOA_DRONE_CREST_MAX 5.5f    // reject more impulse-like false drones
#define DOA_DRONE_CONF_MIN  0.28f   // ignore weak TDOA locks
#define DOA_ONSET_K         4.0f
#define DOA_ONSET_ABS       40.0f
#define DOA_REFRACTORY      4800u    // ~100 ms — allows cat/dog cadence
#define DOA_STREAM_CHUNK    512u
#define DOA_BOUT_GAP        120000u  // ~2.5 s silence ends a walking bout
#define DOA_BOUT_MIN_STEPS  3u
#define DOA_BOUT_MAX_STEPS  16u
#define DOA_WALK_CAD_MIN_HZ 0.95f
#define DOA_WALK_CAD_MAX_HZ 5.8f
#define DOA_WALK_EL_MAX     25.0f   // walkers near horizon / below

#define DOA_MAX_DRONE       2
#define DOA_MAX_VEHICLE     2
#define DOA_MAX_BIRD        2
#define DOA_GATE_DEG        30.0f
#define DOA_TRACK_TTL       12u
#define DOA_VEH_MIN_FRAMES  5u       // ~1.0 s continuous before vehicle entity
#define DOA_VEH_GAP_FRAMES  3u
#define DOA_VEH_RMS         6.5f
#define DOA_VEH_CREST_MAX   4.2f     // continuous pass-by, not footfall spikes
#define DOA_VEH_CREST_MIN   1.2f     // reject near-DC / clipped weirdness
#define DOA_BIRD_RMS        2.2f
#define DOA_BIRD_MIN_FRAMES 3u
#define DOA_BIRD_GAP_FRAMES 2u
#define DOA_BIRD_CREST_MIN  3.0f     // tonal / chirpy
#define DOA_CLASS_MARGIN    0.45f    // min score gap to prefer a specific subclass

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
volatile uint32_t g_doa_nbird;
volatile uint32_t g_doa_entity_id;
volatile uint32_t g_doa_wind;
volatile float    g_doa_wind_az;
volatile float    g_doa_wind_el;
volatile float    g_doa_wind_db;

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
// Bird song / call band ~2–8 kHz (+ 4 kHz split for species cues).
static const biquad_coef_t k_hpf2000 = {
    8.3089802127e-01f, -1.6617960425e+00f, 8.3089802127e-01f,
    -1.6329931619e+00f, 6.9059892324e-01f
};
static const biquad_coef_t k_lpf8000 = {
    1.5505102572e-01f, 3.1010205144e-01f, 1.5505102572e-01f,
    -6.2020410289e-01f, 2.4040820577e-01f
};
static const biquad_coef_t k_hpf4000 = {
    6.8930616877e-01f, -1.3786123375e+00f, 6.8930616877e-01f,
    -1.2796324250e+00f, 4.7759225007e-01f
};
static const biquad_coef_t k_lpf4000 = {
    4.9489956269e-02f, 9.8979912537e-02f, 4.9489956269e-02f,
    -1.2796324250e+00f, 4.7759225007e-01f
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
    CLS_ICE      = ENT_ICE,
    CLS_EV       = ENT_EV,
    CLS_BIRD     = ENT_BIRD,
    CLS_SONGBIRD = ENT_SONGBIRD,
    CLS_CORVID   = ENT_CORVID
} src_class_t;

static const char *class_name(src_class_t c) {
    if (c == CLS_DRONE) return "drone";
    if (c == CLS_NONE) return "none";
    return entity_class_name((entity_class_t)c);
}

static det_class_t det_from_src(src_class_t c) {
    switch (c) {
        case CLS_DRONE:    return DET_DRONE;
        case CLS_HUMAN:    return DET_HUMAN;
        case CLS_CAT:      return DET_CAT;
        case CLS_DOG:      return DET_DOG;
        case CLS_ICE:      return DET_ICE;
        case CLS_EV:       return DET_EV;
        case CLS_BIRD:     return DET_BIRD;
        case CLS_SONGBIRD: return DET_SONGBIRD;
        case CLS_CORVID:   return DET_CORVID;
        default:           return DET_NONE;
    }
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
static track_t g_birds[DOA_MAX_BIRD];
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

static int track_find_bird(float az, float el) {
    int best = -1;
    float best_d = DOA_GATE_DEG;
    for (int i = 0; i < DOA_MAX_BIRD; i++) {
        if (!g_birds[i].used) continue;
        float d = ang_diff_deg(az, g_birds[i].az) + fabsf(el - g_birds[i].el);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static int track_alloc_bird(void) {
    for (int i = 0; i < DOA_MAX_BIRD; i++) if (!g_birds[i].used) return i;
    int oldest = 0;
    for (int i = 1; i < DOA_MAX_BIRD; i++)
        if (g_birds[i].age > g_birds[oldest].age) oldest = i;
    return oldest;
}

static void track_age_birds(void) {
    for (int i = 0; i < DOA_MAX_BIRD; i++) {
        if (!g_birds[i].used) continue;
        g_birds[i].age++;
        if (g_birds[i].age > DOA_TRACK_TTL) g_birds[i].used = false;
    }
}

static uint32_t track_count_birds(void) {
    uint32_t n = 0;
    for (int i = 0; i < DOA_MAX_BIRD; i++) if (g_birds[i].used) n++;
    return n;
}

static void bird_upsert(src_class_t cls, float az, float el, float conf,
                        float lvl_db, uint32_t eid, float match, float chirp_hz) {
    int i = track_find_bird(az, el);
    if (i < 0) i = track_alloc_bird();
    if (g_birds[i].used) {
        g_birds[i].az = 0.7f * g_birds[i].az + 0.3f * az;
        g_birds[i].el = 0.7f * g_birds[i].el + 0.3f * el;
        g_birds[i].conf = 0.6f * g_birds[i].conf + 0.4f * conf;
        g_birds[i].lvl_db = 0.6f * g_birds[i].lvl_db + 0.4f * lvl_db;
    } else {
        g_birds[i].az = az;
        g_birds[i].el = el;
        g_birds[i].conf = conf;
        g_birds[i].lvl_db = lvl_db;
    }
    g_birds[i].used = true;
    g_birds[i].cls = cls;
    g_birds[i].age = 0;
    g_birds[i].entity_id = eid;
    g_birds[i].match = match;
    g_birds[i].cadence_hz = chirp_hz;
    g_birds[i].have_xy = false;
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

// Wind estimate from per-mic LF energy (same LPF250 used by the drone wind gate).
typedef struct {
    bool present;
    float az;
    float el;
    float intensity_db;
    float e_wind[6];
} wind_est_t;

static wind_est_t estimate_wind_from_energy(const float e_wind[6]) {
    wind_est_t w;
    memset(&w, 0, sizeof(w));
    float wx = 0.0f, wy = 0.0f, wz = 0.0f, esum = 0.0f;
    for (int c = 0; c < 6; c++) {
        w.e_wind[c] = e_wind[c];
        float e = e_wind[c];
        esum += e;
        wx += MIC_DIR[c][0] * e;
        wy += MIC_DIR[c][1] * e;
        wz += MIC_DIR[c][2] * e;
    }
    float nsam = (float)(DOA_N - DOA_FILT_SETTLE);
    float rms = sqrtf(esum / (6.0f * nsam + 1e-6f));
    w.intensity_db = 20.0f * log10f((rms + 1e-6f) / 32768.0f);
    w.present = (rms >= DOA_WIND_RMS_MIN);
    float norm = sqrtf(wx * wx + wy * wy + wz * wz) + 1e-9f;
    float dx = wx / norm, dy = wy / norm, dz = wz / norm;
    float az = atan2f(dy, dx) * (180.0f / (float)M_PI);
    if (az < 0.0f) az += 360.0f;
    float el = asinf(fmaxf(-1.0f, fminf(1.0f, dz))) * (180.0f / (float)M_PI);
    w.az = az;
    w.el = el;
    return w;
}

static float prepare_drone_band(bool active[6], wind_est_t *wind_out) {
    int nactive = 0;
    float crest_num = 0.0f, crest_den = 0.0f;
    float e_wind_ch[6];
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
        e_wind_ch[c] = e_wind;
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) e_corr += g_work[c][n] * g_work[c][n];
        g_energy[c] = e_corr;
        // Keep wind gate: reject mic if LF wind energy dominates the drone band.
        float rms = sqrtf(e_bp / (float)(DOA_N - DOA_FILT_SETTLE));
        active[c] = (rms > DOA_DRONE_RMS) && (e_bp >= DOA_WIND_RATIO * (e_wind + 1e-6f));
        if (active[c]) nactive++;
        crest_num += peak;
        crest_den += rms + 1e-6f;
    }
    if (wind_out) *wind_out = estimate_wind_from_energy(e_wind_ch);
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

// Soft membership helpers for class scores (0..1 ramps).
static float score_above(float x, float lo, float hi) {
    if (x <= lo) return 0.0f;
    if (x >= hi) return 1.0f;
    return (x - lo) / (hi - lo);
}
static float score_below(float x, float lo, float hi) {
    if (x >= hi) return 0.0f;
    if (x <= lo) return 1.0f;
    return (hi - x) / (hi - lo);
}
static float score_band(float x, float a, float b, float c, float d) {
    // 0 outside [a,d], 1 inside [b,c], linear ramps a→b and c→d.
    if (x <= a || x >= d) return 0.0f;
    if (x < b) return (x - a) / (b - a + 1e-6f);
    if (x > c) return (d - x) / (d - c + 1e-6f);
    return 1.0f;
}

typedef struct {
    float cadence_hz;
    float ipi_cv;     // inter-onset interval coef. of variation (regularity)
    float low_r;
    float mid_r;
    float high_r;
    float peak_db;
    float crest;
    float el_deg;
} walk_cls_feat_t;

static void walk_feats_from_bout(const bout_t *b, walk_cls_feat_t *f) {
    memset(f, 0, sizeof(*f));
    float ipi_sum = 0.0f, ipi2 = 0.0f;
    uint32_t nipi = 0;
    for (uint32_t i = 1; i < b->n_steps; i++) {
        uint32_t d = b->onset_pos[i] - b->onset_pos[i - 1];
        if (d > 3000u && d < 90000u) { // ~60 ms … ~1.9 s
            float x = (float)d;
            ipi_sum += x;
            ipi2 += x * x;
            nipi++;
        }
    }
    float ipi = nipi ? (ipi_sum / (float)nipi) : (float)DOA_FS_HZ;
    f->cadence_hz = DOA_FS / (ipi + 1.0f);
    if (nipi >= 2u) {
        float mean = ipi_sum / (float)nipi;
        float var = ipi2 / (float)nipi - mean * mean;
        if (var < 0.0f) var = 0.0f;
        f->ipi_cv = sqrtf(var) / (mean + 1e-6f);
    } else {
        f->ipi_cv = 1.0f; // unknown → no regularity bonus
    }
    float n = (float)b->n_steps;
    float peak = b->peak_sum / n;
    f->peak_db = 20.0f * log10f((peak + 1e-6f) / 32768.0f);
    f->low_r = b->e_low / (b->e_full + 1e-6f);
    f->high_r = b->e_high / (b->e_full + 1e-6f);
    f->mid_r = fmaxf(0.0f, 1.0f - f->low_r - f->high_r);
    f->crest = b->crest_sum / n;
    f->el_deg = b->el_sum / n;
}

static src_class_t classify_species(const walk_cls_feat_t *f) {
    float sh = 0.0f, sd = 0.0f, sc = 0.0f;

    // Cadence: soft bands (human slow, dog mid, cat fast).
    sh += 3.0f * score_band(f->cadence_hz, 1.0f, 1.3f, 2.2f, 2.7f);
    sd += 2.6f * score_band(f->cadence_hz, 1.6f, 2.0f, 3.4f, 4.4f);
    sc += 3.0f * score_band(f->cadence_hz, 2.4f, 3.0f, 4.8f, 5.8f);

    // Regular gait → human/dog; irregular light steps → cat.
    sh += 1.4f * score_below(f->ipi_cv, 0.12f, 0.28f);
    sd += 0.8f * score_band(f->ipi_cv, 0.15f, 0.22f, 0.40f, 0.55f);
    sc += 1.2f * score_above(f->ipi_cv, 0.28f, 0.50f);

    // Spectral: human LF thump, dog balanced, cat HF click/patter.
    sh += 2.2f * score_above(f->low_r, 0.34f, 0.50f);
    sh += 1.0f * score_below(f->high_r, 0.10f, 0.22f);
    sd += 1.6f * score_band(f->low_r, 0.22f, 0.30f, 0.42f, 0.52f);
    sd += 1.4f * score_band(f->high_r, 0.16f, 0.22f, 0.34f, 0.42f);
    sc += 2.4f * score_above(f->high_r, 0.28f, 0.42f);
    sc += 1.0f * score_below(f->low_r, 0.18f, 0.32f);
    sd += 0.6f * score_band(f->mid_r, 0.10f, 0.18f, 0.35f, 0.45f);

    // Level / crest.
    sh += 1.6f * score_above(f->peak_db, -42.0f, -32.0f);
    sd += 1.2f * score_band(f->peak_db, -50.0f, -44.0f, -36.0f, -30.0f);
    sc += 1.8f * score_below(f->peak_db, -52.0f, -42.0f);
    sh += 0.6f * score_above(f->crest, 4.2f, 6.0f);
    sc += 0.8f * score_above(f->crest, 5.5f, 8.0f);

    // Geometry prior: walkers near/below horizon.
    float el_ok = score_below(fabsf(f->el_deg), 8.0f, DOA_WALK_EL_MAX);
    sh += 0.8f * el_ok;
    sd += 0.8f * el_ok;
    sc += 0.5f * el_ok;

    if (sh >= sd && sh >= sc) return CLS_HUMAN;
    if (sd >= sc) return CLS_DOG;
    return CLS_CAT;
}

static void sig_from_walk_feat(const walk_cls_feat_t *f, entity_sig_t *s) {
    s->cadence_hz = f->cadence_hz;
    s->peak_db = f->peak_db;
    s->low_ratio = f->low_r;
    s->high_ratio = f->high_r;
    s->mid_ratio = f->mid_r;
    s->crest = f->crest;
    // Pack regularity into az_n unused? Keep az/el from bout averages separately.
    s->az_n = 0.0f;
    s->el_n = (f->el_deg + 90.0f) / 180.0f;
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

    walk_cls_feat_t wf;
    walk_feats_from_bout(&g_bout, &wf);
    float n = (float)g_bout.n_steps;
    float az = g_bout.az_sum / n;
    float el = g_bout.el_sum / n;
    float conf = g_bout.conf_sum / n;
    float lvl = g_bout.lvl_sum / n;

    // Reject non-walker bouts (cadence / elevation outside walking priors).
    if (wf.cadence_hz < DOA_WALK_CAD_MIN_HZ || wf.cadence_hz > DOA_WALK_CAD_MAX_HZ ||
        el > DOA_WALK_EL_MAX) {
        g_bout.active = false;
        g_bout.n_steps = 0;
        return;
    }

    src_class_t cls = classify_species(&wf);
    entity_sig_t sig;
    sig_from_walk_feat(&wf, &sig);
    sig.az_n = az / 360.0f;
    sig.el_n = (el + 90.0f) / 180.0f;

    float match = 0.0f;
    uint32_t eid = entity_store_match_or_create((entity_class_t)cls, &sig, &match);

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

    (void)detection_log_observe(det_from_src(cls), eid, az, el, lvl, conf);

    if (dbg_log_enabled()) {
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
    }

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
    // Reject impulsive (footsteps) and near-empty / pathological crest.
    if (f.crest > DOA_VEH_CREST_MAX || f.crest < DOA_VEH_CREST_MIN) {
        for (int c = 0; c < 6; c++) active[c] = false;
    }
    return f;
}

static src_class_t classify_vehicle(float low_r, float mid_r, float high_r,
                                    float peak_db, float env_var, float crest,
                                    float daz_deg) {
    // Soft ICE vs EV scores + pass-by / continuity cues.
    float s_ice = 0.0f, s_ev = 0.0f;

    // ICE: LF engine rumble, rougher envelope, often louder.
    s_ice += 2.8f * score_above(low_r, 0.28f, 0.48f);
    s_ice += 1.2f * score_below(mid_r, 0.12f, 0.28f);
    s_ice += 1.4f * score_above(env_var, 120.0f, 320.0f);
    s_ice += 1.2f * score_above(peak_db, -40.0f, -28.0f);
    s_ice += 0.6f * score_band(crest, 1.4f, 1.8f, 3.2f, 4.0f);

    // EV: weaker LF, tire/whine mid-high, smoother envelope, quieter.
    s_ev += 2.4f * score_below(low_r, 0.18f, 0.36f);
    s_ev += 1.8f * score_above(mid_r, 0.20f, 0.38f);
    s_ev += 1.6f * score_above(high_r, 0.18f, 0.36f);
    s_ev += 1.2f * score_below(peak_db, -48.0f, -34.0f);
    s_ev += 1.0f * score_below(env_var, 40.0f, 180.0f);
    s_ev += 0.6f * score_band(crest, 1.3f, 1.6f, 2.8f, 3.6f);

    // Moving pass-by supports either class; slight ICE bias (exhaust Doppler).
    float move = score_above(daz_deg, 6.0f, 25.0f);
    s_ice += 0.7f * move;
    s_ev += 0.5f * move;

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
    // Reject walker-like / non-vehicle spectral piles (LF+HF without mid body).
    if (crest > 3.8f && daz < 5.0f) {
        g_veh.active = false;
        g_veh.frames = 0;
        g_veh.quiet = 0;
        return;
    }
    if (low_r < 0.12f && mid_r < 0.12f) {
        g_veh.active = false;
        g_veh.frames = 0;
        g_veh.quiet = 0;
        return;
    }

    // Modulation proxy from envelope variance (not a true Hz; stored in cadence slot).
    float mod_hz = sqrtf(env_var + 1.0f) * 0.01f;
    if (daz > 15.0f) mod_hz += 0.5f; // moving pass-by bonus

    src_class_t cls = classify_vehicle(low_r, mid_r, high_r, lvl, env_var, crest, daz);

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
    (void)detection_log_observe(det_from_src(cls), eid, az, el, lvl, conf);

    if (dbg_log_enabled()) {
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
    }

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
// Bird song / calls (~2–8 kHz) — species heuristic + DOA position
// ---------------------------------------------------------------------------
typedef struct {
    bool active;
    uint32_t frames;
    uint32_t quiet;
    float az_sum, el_sum, conf_sum, lvl_sum;
    float e_full, e_lo, e_hi;
    float crest_sum;
    float zcr_sum;   // zero-crossing rate proxy → chirp brightness
} bird_bout_t;

static bird_bout_t g_bird;

typedef struct {
    float e_full, e_lo, e_hi;
    float crest;
    float zcr;
    float peak;
} bird_feat_t;

static bird_feat_t prepare_bird_band(bool active[6]) {
    bird_feat_t f;
    memset(&f, 0, sizeof(f));
    float peak_all = 0.0f, rms_all = 0.0f;
    for (int c = 0; c < 6; c++) {
        float mean = 0.0f;
        for (uint32_t n = 0; n < DOA_N; n++) mean += g_work[c][n];
        mean /= (float)DOA_N;
        biquad_mem_t hp = {0}, lp = {0}, lo = {0}, hi = {0};
        float e_bp = 0.0f, e_lo = 0.0f, e_hi = 0.0f, peak = 0.0f;
        float prev = 0.0f;
        uint32_t zc = 0, zn = 0;
        for (uint32_t n = 0; n < DOA_N; n++) {
            float x = g_work[c][n] - mean;
            float y = biquad_step(&k_hpf2000, &hp, x);
            y = biquad_step(&k_lpf8000, &lp, y);
            float yl = biquad_step(&k_lpf4000, &lo, y);
            float yh = biquad_step(&k_hpf4000, &hi, y);
            g_work[c][n] = y;
            if (n >= DOA_FILT_SETTLE) {
                e_bp += y * y;
                e_lo += yl * yl;
                e_hi += yh * yh;
                float ay = fabsf(y);
                if (ay > peak) peak = ay;
                if ((y >= 0.0f && prev < 0.0f) || (y < 0.0f && prev >= 0.0f)) zc++;
                prev = y;
                zn++;
            }
        }
        float e_corr = 0.0f;
        for (uint32_t n = DOA_CORR_LO; n < DOA_CORR_HI; n++) e_corr += g_work[c][n] * g_work[c][n];
        g_energy[c] = e_corr;
        float rms = sqrtf(e_bp / (float)(DOA_N - DOA_FILT_SETTLE));
        active[c] = rms > DOA_BIRD_RMS;
        peak_all += peak;
        rms_all += rms;
        f.e_full += e_bp;
        f.e_lo += e_lo;
        f.e_hi += e_hi;
        if (peak > f.peak) f.peak = peak;
        if (c == 0 && zn) f.zcr = (float)zc / (float)zn;
    }
    f.crest = peak_all / (rms_all + 1e-6f);
    if (f.crest < DOA_BIRD_CREST_MIN) {
        for (int c = 0; c < 6; c++) active[c] = false;
    }
    return f;
}

static src_class_t classify_bird(float lo_r, float hi_r, float crest, float zcr,
                                 float lvl_db, float el_deg, uint32_t frames) {
    float ss = 0.0f, sc = 0.0f, sg = 0.8f;

    // Songbird: bright (>4 kHz), higher ZCR, chirpy crest.
    ss += 2.8f * score_above(hi_r, 0.30f, 0.50f);
    ss += 1.8f * score_above(zcr, 0.08f, 0.16f);
    ss += 1.4f * score_above(crest, 3.4f, 5.5f);
    ss += 0.8f * score_below(lvl_db, -48.0f, -36.0f);

    // Corvid: heavier below 4 kHz, lower ZCR, often louder / harsher.
    sc += 2.8f * score_above(lo_r, 0.38f, 0.58f);
    sc += 1.4f * score_below(zcr, 0.04f, 0.11f);
    sc += 1.0f * score_below(crest, 2.8f, 4.2f);
    sc += 1.0f * score_above(lvl_db, -42.0f, -30.0f);

    // Geometry / persistence: birds often elevated; longer bouts → more confident.
    float el_boost = score_above(el_deg, -5.0f, 25.0f);
    ss += 0.6f * el_boost;
    sc += 0.5f * el_boost;
    sg += 0.4f * el_boost;
    if (frames >= 5u) { ss += 0.3f; sc += 0.3f; }

    // Ambiguous → generic bird (avoid overconfident species tags).
    float top = fmaxf(ss, sc);
    if (fabsf(ss - sc) < DOA_CLASS_MARGIN && top < sg + 1.2f) return CLS_BIRD;
    if (ss >= sc && ss >= sg + DOA_CLASS_MARGIN) return CLS_SONGBIRD;
    if (sc >= ss && sc >= sg + DOA_CLASS_MARGIN) return CLS_CORVID;
    if (ss >= sc && ss >= sg) return CLS_SONGBIRD;
    if (sc >= sg) return CLS_CORVID;
    return CLS_BIRD;
}

static void finalize_bird_bout(void) {
    if (!g_bird.active || g_bird.frames < DOA_BIRD_MIN_FRAMES) {
        g_bird.active = false;
        g_bird.frames = 0;
        return;
    }
    float n = (float)g_bird.frames;
    float az = g_bird.az_sum / n;
    float el = g_bird.el_sum / n;
    float conf = g_bird.conf_sum / n;
    float lvl = g_bird.lvl_sum / n;
    float lo_r = g_bird.e_lo / (g_bird.e_full + 1e-6f);
    float hi_r = g_bird.e_hi / (g_bird.e_full + 1e-6f);
    float crest = g_bird.crest_sum / n;
    float zcr = g_bird.zcr_sum / n;
    float chirp_hz = zcr * (DOA_FS * 0.5f) / 4000.0f; // rough brightness proxy

    // Reject non-bird spectral piles (almost no split energy).
    if (lo_r + hi_r < 0.35f) {
        g_bird.active = false;
        g_bird.frames = 0;
        g_bird.quiet = 0;
        return;
    }

    src_class_t cls = classify_bird(lo_r, hi_r, crest, zcr, lvl, el, g_bird.frames);
    entity_sig_t sig;
    sig.cadence_hz = chirp_hz;
    sig.peak_db = lvl;
    sig.low_ratio = lo_r;
    sig.mid_ratio = 0.0f;
    sig.high_ratio = hi_r;
    sig.crest = crest;
    sig.az_n = az / 360.0f;
    sig.el_n = (el + 90.0f) / 180.0f;

    float match = 0.0f;
    uint32_t eid = entity_store_match_or_create((entity_class_t)cls, &sig, &match);
    bird_upsert(cls, az, el, conf, lvl, eid, match, chirp_hz);
    g_doa_entity_id = eid;
    g_doa_nbird = track_count_birds();
    (void)detection_log_observe(det_from_src(cls), eid, az, el, lvl, conf);

    if (dbg_log_enabled()) {
        uint32_t lock = dbg_line_lock();
        dbg_puts("ENTITY id=");
        dbg_putu32(eid);
        dbg_puts(" class=");
        dbg_puts(class_name(cls));
        dbg_puts(" frames=");
        dbg_putu32(g_bird.frames);
        dbg_puts(" match=");
        put_f2(match);
        dbg_puts(" az=");
        put_f1(az);
        dbg_puts(" el=");
        put_f1(el);
        dbg_puts(" (bird position)\n");
        dbg_line_unlock(lock);
    }

    g_bird.active = false;
    g_bird.frames = 0;
    g_bird.quiet = 0;
}

static void analyse_bird(uint32_t h) {
    bool active[6];
    load_raw_window(h);
    bird_feat_t feat = prepare_bird_band(active);
    doa_fix_t r = solve_tdoa(active);
    if (!r.ok) {
        if (g_bird.active) {
            g_bird.quiet++;
            if (g_bird.quiet >= DOA_BIRD_GAP_FRAMES) finalize_bird_bout();
        }
        return;
    }
    // Birds are typically above/around the array — allow wide elevation.
    if (!g_bird.active) {
        memset(&g_bird, 0, sizeof(g_bird));
        g_bird.active = true;
    }
    g_bird.quiet = 0;
    g_bird.frames++;
    g_bird.az_sum += r.az;
    g_bird.el_sum += r.el;
    g_bird.conf_sum += r.conf;
    g_bird.lvl_sum += r.lvl_db;
    g_bird.e_full += feat.e_full;
    g_bird.e_lo += feat.e_lo;
    g_bird.e_hi += feat.e_hi;
    g_bird.crest_sum += feat.crest;
    g_bird.zcr_sum += feat.zcr;
    bird_upsert(CLS_NONE, r.az, r.el, r.conf, r.lvl_db, 0, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static void report_tracks(void) {
    g_doa_ndrone = track_count_drones();
    g_doa_nwalker = g_walker.used ? 1u : 0u;
    g_doa_nvehicle = track_count_vehicles();
    g_doa_nbird = track_count_birds();
    if (g_walker.used && g_walker.entity_id) g_doa_entity_id = g_walker.entity_id;

    // Persist live tracks into DET log (only after TIME SYNC — see detection_log).
    for (int i = 0; i < DOA_MAX_DRONE; i++) {
        if (!g_drones[i].used) continue;
        (void)detection_log_observe(DET_DRONE, 0, g_drones[i].az, g_drones[i].el,
                                    g_drones[i].lvl_db, g_drones[i].conf);
    }
    for (int i = 0; i < DOA_MAX_VEHICLE; i++) {
        if (!g_vehicles[i].used || g_vehicles[i].cls == CLS_NONE) continue;
        (void)detection_log_observe(det_from_src(g_vehicles[i].cls), g_vehicles[i].entity_id,
                                    g_vehicles[i].az, g_vehicles[i].el,
                                    g_vehicles[i].lvl_db, g_vehicles[i].conf);
    }
    for (int i = 0; i < DOA_MAX_BIRD; i++) {
        if (!g_birds[i].used || g_birds[i].cls == CLS_NONE) continue;
        (void)detection_log_observe(det_from_src(g_birds[i].cls), g_birds[i].entity_id,
                                    g_birds[i].az, g_birds[i].el,
                                    g_birds[i].lvl_db, g_birds[i].conf);
    }
    if (g_walker.used && g_walker.cls != CLS_NONE) {
        (void)detection_log_observe(det_from_src(g_walker.cls), g_walker.entity_id,
                                    g_walker.az, g_walker.el,
                                    g_walker.lvl_db, g_walker.conf);
    }
    if (g_doa_wind) {
        (void)detection_log_observe(DET_WIND, 0, g_doa_wind_az, g_doa_wind_el,
                                    g_doa_wind_db, 0.5f);
    }

    if (!dbg_log_enabled()) {
        g_doa_out++;
        return;
    }

    uint32_t lock = dbg_line_lock();
    if (g_doa_wind) {
        dbg_puts("SRC class=wind az=");
        put_f1(g_doa_wind_az);
        dbg_puts(" el=");
        put_f1(g_doa_wind_el);
        dbg_puts(" inten=");
        put_f1(g_doa_wind_db);
        dbg_puts("dB\n");
    }
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
    for (int i = 0; i < DOA_MAX_BIRD; i++) {
        if (!g_birds[i].used) continue;
        dbg_puts("SRC class=");
        dbg_puts(g_birds[i].cls == CLS_NONE ? "bird" : class_name(g_birds[i].cls));
        dbg_puts(" entity=");
        dbg_putu32(g_birds[i].entity_id);
        dbg_puts(" az="); put_f1(g_birds[i].az);
        dbg_puts(" el="); put_f1(g_birds[i].el);
        dbg_puts(" conf="); put_f1(g_birds[i].conf);
        dbg_puts(" lvl="); put_f1(g_birds[i].lvl_db);
        dbg_puts("dB");
        if (g_birds[i].cls != CLS_NONE) {
            dbg_puts(" match=");
            put_f2(g_birds[i].match);
        }
        dbg_puts(" pos=az/el\n");
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
    dbg_puts(" bird=");
    dbg_putu32(g_doa_nbird);
    dbg_puts(" walker=");
    dbg_putu32(g_doa_nwalker);
    dbg_puts(" wind=");
    dbg_putu32(g_doa_wind);
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
    wind_est_t wind;
    load_raw_window(h);
    (void)prepare_drone_band(active, &wind);

    // Keep wind gate for drones; also publish wind intensity + steered direction.
    if (wind.present) {
        g_doa_wind = 1;
        g_doa_wind_az = wind.az;
        g_doa_wind_el = wind.el;
        g_doa_wind_db = wind.intensity_db;
    } else {
        g_doa_wind = 0;
    }

    doa_fix_t primary = solve_tdoa(active);
    if (!primary.ok || primary.conf < DOA_DRONE_CONF_MIN) return;

    // Prefer airborne / not deep ground clutter for drone locks.
    if (primary.el < -50.0f) return;

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
        float conf2 = secondary.conf * 0.85f;
        if (secondary.ok && conf2 >= DOA_DRONE_CONF_MIN && secondary.el >= -50.0f &&
            (ang_diff_deg(secondary.az, primary.az) +
             fabsf(secondary.el - primary.el)) > DOA_GATE_DEG) {
            drone_upsert(secondary.az, secondary.el, conf2, secondary.lvl_db);
        }
    }
}

static void analyse_walker_onset(uint32_t h) {
    bool active[6];
    load_raw_window(h);
    walk_feat_t feat = prepare_walker_band(active);
    doa_fix_t r = solve_tdoa(active);
    if (!r.ok) return;
    // Early geometry gate — footsteps are near/below horizon.
    if (r.el > DOA_WALK_EL_MAX) return;
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
    detection_log_core_init();

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
    memset(&g_bird, 0, sizeof(g_bird));
    memset(g_vehicles, 0, sizeof(g_vehicles));
    memset(g_birds, 0, sizeof(g_birds));
    g_doa_wind = 0;

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
            track_age_birds();
            if (g_walker.used) {
                g_walker.age++;
                if (g_walker.age > DOA_TRACK_TTL) {
                    g_walker.used = false;
                    g_doa_nwalker = 0;
                }
            }
            analyse_drone(h);
            analyse_vehicle(h);
            analyse_bird(h);
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
