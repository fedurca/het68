// neo_srp.c — see neo_srp.h.
//
// Direction comes from wideband SRP-PHAT (600 Hz–8 kHz), not from the phase of
// the blade line. On one frequency the direct sound and every reflection
// (ground, walls) add into a single phasor whose phase points nowhere. Across
// a wide band each path forms its own peak. The tracker follows the peak it
// holds, never steps onto the ground image below it, and moves to another peak
// only after 1 s of clearly stronger evidence there.
#include "neo_srp.h"

#include <math.h>
#include <string.h>

#define FS_HZ          48000.0f
#define PI_F           3.14159265358979f
#define DEG            (PI_F / 180.0f)

#define SRP_K_LO       13      // 609 Hz
#define SRP_K_HI       170     // 7969 Hz
#define SRP_NK         (SRP_K_HI - SRP_K_LO + 1)
#define SRP_NPAIR      15
#define SRP_T_S        0.25f   // cross-spectrum memory
#define SRP_LAG_DIV    4       // lag grid in 1/4 samples
#define SRP_M_MAX      264     // lag half-range for a cube edge up to 400 mm
#define SRP_TAB        (4u * NEO_SRP_N)
#define SRP_C_MIN      300.0f

#define GRID_AZ        90      // 0..356°
#define GRID_EL        44      // −84..+88°
#define GRID_STEP      4.0f
#define GRID_EL0       (-84.0f)
#define MAX_CAND       48
#define MAX_LOBES      6
#define LOBE_SEP_DEG   12.0f

// Tuned on the 2026-09-28 flights: 20 m hover, 1 m and 3 m circles, and a
// flight with a vacuum running.
#define TRK_GATE_DEG   25.0f
#define TRK_Q_ACQ      0.15f
#define TRK_Q_HOLD     0.10f
#define TRK_UP_RATIO   0.80f
#define TRK_UP_DAZ     35.0f
#define TRK_UP_DEL     20.0f
#define TRK_JUMP_RATIO 1.40f
#define TRK_JUMP_N     5
#define TRK_LOST_N     5
#define TRK_TTL        8
#define TRK_ACQ_N      3
#define TRK_ACQ_DEG    15.0f
#define TRK_SMOOTH     0.5f
#define TRK_ANTI_DEG   30.0f

// Motors off: prominence < 14 and RMS < 100. In flight the line sits near
// 1.0–1.2 kHz, 100–1000× above its neighbours.
#define TONE_F_LO      900.0f
#define TONE_F_STEP    20.0f
#define TONE_NF        24
#define TONE_PROM_MIN  40.0f   // hover can dip below the old 50
#define TONE_RMS_MIN   50.0f   // close hover on one loud mic is still a Neo
// A 70–130 Hz hum is common outdoors. A vacuum's blade-band energy is only a
// weak harmonic of that hum. A real Neo blade line is sharp (high prom) even
// when a motor/mains peak sits near a subharmonic — so require the LF tone to
// dominate the blade bin and keep blade prominence modest before vetoing.
#define VAC_PROM_MIN   40.0f
#define VAC_RATIO_MIN  3.0f
#define VAC_PROM_MAX   100.0f  // above this, treat as Neo blade, not vacuum

static float s_sin[SRP_TAB];
static uint16_t s_rev[NEO_SRP_N];
static float s_win[NEO_SRP_N];
static float s_re[NEO_SRP_N];
static float s_im[NEO_SRP_N];
static float s_yre[6][SRP_NK];
static float s_yim[6][SRP_NK];
static float s_are[SRP_NPAIR][SRP_NK];
static float s_aim[SRP_NPAIR][SRP_NK];
static float s_accw;
static float s_alpha;
static uint8_t s_pi[SRP_NPAIR];
static uint8_t s_pj[SRP_NPAIR];
static float s_dz[SRP_NPAIR];
static float s_pre[SRP_NPAIR][GRID_AZ];
static float s_caz[GRID_AZ];
static float s_saz[GRID_AZ];
static float s_cel[GRID_EL];
static float s_sel[GRID_EL];
static int s_m;
static float s_r[SRP_NPAIR][2 * SRP_M_MAX + 2];
static float s_q[GRID_AZ * GRID_EL];
static float s_tone_buf[NEO_TONE_N];

typedef struct {
    bool active;
    float u[3];
    float q;
    int cand_g;
    uint8_t cand_n;
    uint8_t tone_bits;
    uint8_t jump_n;
    uint8_t lost_n;
    uint8_t absent;
} tracker_t;

static tracker_t s_trk;

static void fft(float *re, float *im) {
    const uint32_t n = NEO_SRP_N;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = s_rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (uint32_t len = 2u; len <= n; len <<= 1) {
        const uint32_t half = len >> 1;
        const uint32_t tstep = SRP_TAB / len;
        for (uint32_t i = 0; i < n; i += len) {
            for (uint32_t k = 0; k < half; k++) {
                const uint32_t ti = k * tstep;
                const float wr = s_sin[(ti + SRP_TAB / 4u) & (SRP_TAB - 1u)];
                const float wi = -s_sin[ti];
                const uint32_t a = i + k;
                const uint32_t b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

void neo_srp_init(const float mic_pos[6][3]) {
    for (uint32_t i = 0; i < SRP_TAB; i++)
        s_sin[i] = sinf(2.0f * PI_F * (float)i / (float)SRP_TAB);
    for (uint32_t i = 0; i < NEO_SRP_N; i++) {
        uint32_t r = 0, x = i;
        for (uint32_t b = 1u; b < NEO_SRP_N; b <<= 1) {
            r = (r << 1) | (x & 1u);
            x >>= 1;
        }
        s_rev[i] = (uint16_t)r;
        s_win[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)(NEO_SRP_N - 1u));
    }
    for (int a = 0; a < GRID_AZ; a++) {
        float az = GRID_STEP * (float)a * DEG;
        s_caz[a] = cosf(az);
        s_saz[a] = sinf(az);
    }
    for (int e = 0; e < GRID_EL; e++) {
        float el = (GRID_EL0 + GRID_STEP * (float)e) * DEG;
        s_cel[e] = cosf(el);
        s_sel[e] = sinf(el);
    }
    float dmax = 0.0f;
    int p = 0;
    for (int i = 0; i < 6; i++) {
        for (int j = i + 1; j < 6; j++) {
            float dx = mic_pos[i][0] - mic_pos[j][0];
            float dy = mic_pos[i][1] - mic_pos[j][1];
            float dz = mic_pos[i][2] - mic_pos[j][2];
            s_pi[p] = (uint8_t)i;
            s_pj[p] = (uint8_t)j;
            s_dz[p] = dz;
            for (int a = 0; a < GRID_AZ; a++) s_pre[p][a] = dx * s_caz[a] + dy * s_saz[a];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > dmax) dmax = d;
            p++;
        }
    }
    s_m = (int)ceilf((dmax * FS_HZ / SRP_C_MIN + 2.0f) * (float)SRP_LAG_DIV);
    if (s_m > SRP_M_MAX) s_m = SRP_M_MAX;
    s_alpha = expf(-(float)NEO_SRP_HOP / (FS_HZ * SRP_T_S));
    memset(s_are, 0, sizeof(s_are));
    memset(s_aim, 0, sizeof(s_aim));
    s_accw = 0.0f;
    memset(&s_trk, 0, sizeof(s_trk));
}

void neo_srp_frame(const neo_ring_t *r, uint32_t start) {
    const uint32_t n = NEO_SRP_N;
    // Two real channels per complex FFT.
    for (int c = 0; c < 6; c += 2) {
        float ma = 0.0f, mb = 0.0f;
        for (uint32_t i = 0; i < n; i++) {
            const volatile int16_t *s = r->s[(start + i) & r->mask];
            float a = (float)s[c];
            float b = (float)s[c + 1];
            s_re[i] = a;
            s_im[i] = b;
            ma += a;
            mb += b;
        }
        ma /= (float)n;
        mb /= (float)n;
        for (uint32_t i = 0; i < n; i++) {
            s_re[i] = (s_re[i] - ma) * s_win[i];
            s_im[i] = (s_im[i] - mb) * s_win[i];
        }
        fft(s_re, s_im);
        for (int kk = 0; kk < SRP_NK; kk++) {
            const uint32_t k = (uint32_t)(SRP_K_LO + kk);
            const uint32_t nk = n - k;
            float ar = s_re[k] + s_re[nk];
            float ai = s_im[k] - s_im[nk];
            float br = s_im[k] + s_im[nk];
            float bi = s_re[nk] - s_re[k];
            float pa = ar * ar + ai * ai;
            float pb = br * br + bi * bi;
            float ia = pa > 1e-12f ? 1.0f / sqrtf(pa) : 0.0f;
            float ib = pb > 1e-12f ? 1.0f / sqrtf(pb) : 0.0f;
            s_yre[c][kk] = ar * ia;
            s_yim[c][kk] = ai * ia;
            s_yre[c + 1][kk] = br * ib;
            s_yim[c + 1][kk] = bi * ib;
        }
    }
    const float al = s_alpha;
    for (int p = 0; p < SRP_NPAIR; p++) {
        const float *xr = s_yre[s_pi[p]];
        const float *xi = s_yim[s_pi[p]];
        const float *yr = s_yre[s_pj[p]];
        const float *yi = s_yim[s_pj[p]];
        float *ar = s_are[p];
        float *ai = s_aim[p];
        for (int kk = 0; kk < SRP_NK; kk++) {
            ar[kk] = al * ar[kk] + (xr[kk] * yr[kk] + xi[kk] * yi[kk]);
            ai[kk] = al * ai[kk] + (xi[kk] * yr[kk] - xr[kk] * yi[kk]);
        }
    }
    s_accw = al * s_accw + 1.0f;
}

static void srp_map(float c_sound) {
    const int m0 = s_m;
    for (int p = 0; p < SRP_NPAIR; p++) {
        const float *ar = s_are[p];
        const float *ai = s_aim[p];
        float *rp = s_r[p];
        for (int m = -m0; m <= m0; m++) {
            const uint32_t step = (uint32_t)m;
            uint32_t idx = (uint32_t)(SRP_K_LO * m);
            float acc = 0.0f;
            for (int kk = 0; kk < SRP_NK; kk++) {
                const uint32_t i = idx & (SRP_TAB - 1u);
                acc += ar[kk] * s_sin[(i + SRP_TAB / 4u) & (SRP_TAB - 1u)] - ai[kk] * s_sin[i];
                idx += step;
            }
            rp[m + m0] = acc;
        }
    }
    const float scale = FS_HZ * (float)SRP_LAG_DIV / c_sound;
    const float w = s_accw > 1e-6f ? s_accw : 1e-6f;
    const float norm = 1.0f / ((float)(SRP_NPAIR * SRP_NK) * w);
    const int imax = 2 * m0 - 1;
    for (int a = 0; a < GRID_AZ; a++) {
        for (int e = 0; e < GRID_EL; e++) {
            const float ce = s_cel[e];
            const float se = s_sel[e];
            float v = 0.0f;
            for (int p = 0; p < SRP_NPAIR; p++) {
                float fi = (float)m0 - scale * (s_pre[p][a] * ce + s_dz[p] * se);
                int i0 = (int)fi;
                if (i0 < 0) i0 = 0;
                if (i0 > imax) i0 = imax;
                float fr = fi - (float)i0;
                const float *rp = s_r[p];
                v += rp[i0] + fr * (rp[i0 + 1] - rp[i0]);
            }
            s_q[a * GRID_EL + e] = v * norm;
        }
    }
}

static void grid_u(int g, float u[3]) {
    int a = g / GRID_EL;
    int e = g % GRID_EL;
    u[0] = s_cel[e] * s_caz[a];
    u[1] = s_cel[e] * s_saz[a];
    u[2] = s_sel[e];
}

static float grid_az(int g) { return GRID_STEP * (float)(g / GRID_EL); }
static float grid_el(int g) { return GRID_EL0 + GRID_STEP * (float)(g % GRID_EL); }

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float daz(float a, float b) {
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

static void u_azel(const float u[3], float *az, float *el) {
    float a = atan2f(u[1], u[0]) / DEG;
    if (a < 0.0f) a += 360.0f;
    float z = u[2];
    if (z > 1.0f) z = 1.0f;
    if (z < -1.0f) z = -1.0f;
    *az = a;
    *el = asinf(z) / DEG;
}

// Local maxima of the map, strongest first, at least LOBE_SEP_DEG apart.
static int find_lobes(int *out) {
    static int cg[MAX_CAND];
    static float cq[MAX_CAND];
    int nc = 0;
    for (int a = 0; a < GRID_AZ; a++) {
        const int al = (a + GRID_AZ - 1) % GRID_AZ;
        const int ar = (a + 1) % GRID_AZ;
        for (int e = 0; e < GRID_EL; e++) {
            const float v = s_q[a * GRID_EL + e];
            bool peak = true;
            for (int de = -1; de <= 1 && peak; de++) {
                int e2 = e + de;
                if (e2 < 0 || e2 >= GRID_EL) continue;
                if (s_q[al * GRID_EL + e2] > v || s_q[ar * GRID_EL + e2] > v) peak = false;
                if (de != 0 && s_q[a * GRID_EL + e2] > v) peak = false;
            }
            if (!peak) continue;
            if (nc == MAX_CAND && v <= cq[MAX_CAND - 1]) continue;
            int i = (nc < MAX_CAND) ? nc++ : MAX_CAND - 1;
            while (i > 0 && cq[i - 1] < v) {
                cq[i] = cq[i - 1];
                cg[i] = cg[i - 1];
                i--;
            }
            cq[i] = v;
            cg[i] = a * GRID_EL + e;
        }
    }
    const float cs = cosf(LOBE_SEP_DEG * DEG);
    int n = 0;
    for (int i = 0; i < nc && n < MAX_LOBES; i++) {
        float u[3];
        grid_u(cg[i], u);
        bool apart = true;
        for (int j = 0; j < n && apart; j++) {
            float v[3];
            grid_u(out[j], v);
            if (dot3(u, v) >= cs) apart = false;
        }
        if (apart) out[n++] = cg[i];
    }
    return n;
}

// The ground reflection sits below the direct path at nearly the same azimuth.
// Prefer the upper peak of such a pair when it is nearly as strong.
static int pick_upper(const int *l, int n) {
    int best = l[0];
    const float q0 = s_q[l[0]];
    for (int i = 1; i < n; i++) {
        int g = l[i];
        if (daz(grid_az(g), grid_az(best)) <= TRK_UP_DAZ &&
            grid_el(g) >= grid_el(best) + TRK_UP_DEL && s_q[g] >= TRK_UP_RATIO * q0)
            best = g;
    }
    return best;
}

static neo_track_t track_out(const tracker_t *t) {
    neo_track_t o;
    o.active = true;
    u_azel(t->u, &o.az, &o.el);
    o.q = t->q;
    return o;
}

static int popcount4(uint8_t b) {
    return (b & 1) + ((b >> 1) & 1) + ((b >> 2) & 1) + ((b >> 3) & 1);
}

neo_track_t neo_track_update(bool tone, float c_sound_m_s) {
    tracker_t *t = &s_trk;
    neo_track_t none;
    memset(&none, 0, sizeof(none));
    srp_map(c_sound_m_s);
    int l[MAX_LOBES];
    const int nl = find_lobes(l);
    t->tone_bits = (uint8_t)(((t->tone_bits << 1) | (tone ? 1u : 0u)) & 0x0Fu);

    if (!t->active) {
        if (!tone || popcount4(t->tone_bits) < 3 || nl == 0) {
            t->cand_n = 0;
            return none;
        }
        const int g = pick_upper(l, nl);
        if (s_q[g] < TRK_Q_ACQ) {
            t->cand_n = 0;
            return none;
        }
        float u[3];
        grid_u(g, u);
        if (t->cand_n > 0) {
            float v[3];
            grid_u(t->cand_g, v);
            if (dot3(u, v) < cosf(TRK_ACQ_DEG * DEG)) t->cand_n = 0;
        }
        t->cand_g = g;
        t->cand_n++;
        if (t->cand_n < TRK_ACQ_N) return none;
        t->active = true;
        memcpy(t->u, u, sizeof(u));
        t->q = s_q[g];
        t->cand_n = 0;
        t->jump_n = 0;
        t->lost_n = 0;
        t->absent = 0;
        return track_out(t);
    }

    if (!tone) {
        if (++t->absent > TRK_TTL) {
            t->active = false;
            return none;
        }
        return track_out(t);
    }
    t->absent = 0;

    const float cgate = cosf(TRK_GATE_DEG * DEG);
    int in[MAX_LOBES];
    int ni = 0;
    for (int i = 0; i < nl; i++) {
        float v[3];
        grid_u(l[i], v);
        if (dot3(v, t->u) >= cgate) in[ni++] = l[i];
    }
    int gl = -1;
    if (ni > 0) {
        gl = in[0];
        const float q0 = s_q[in[0]];
        for (int i = 1; i < ni; i++) {
            int g = in[i];
            if (daz(grid_az(g), grid_az(gl)) <= TRK_UP_DAZ &&
                grid_el(g) >= grid_el(gl) + TRK_UP_DEL && s_q[g] >= TRK_UP_RATIO * q0)
                gl = g;
        }
    }
    if (gl < 0 || s_q[gl] < TRK_Q_HOLD) {
        if (++t->lost_n >= TRK_LOST_N) {
            t->active = false;
            return none;
        }
        return track_out(t);
    }
    t->lost_n = 0;
    const float ql = s_q[gl];
    float w[3];
    grid_u(gl, w);
    for (int k = 0; k < 3; k++) t->u[k] += TRK_SMOOTH * (w[k] - t->u[k]);
    float mag = sqrtf(dot3(t->u, t->u));
    if (mag > 1e-6f) {
        for (int k = 0; k < 3; k++) t->u[k] /= mag;
    }
    t->q = ql;

    const int gb = pick_upper(l, nl);
    float b[3];
    grid_u(gb, b);
    float taz, tel;
    u_azel(t->u, &taz, &tel);
    const bool ground = daz(grid_az(gb), taz) <= TRK_UP_DAZ && grid_el(gb) < tel - TRK_UP_DEL;
    const bool anti = -dot3(b, t->u) >= cosf(TRK_ANTI_DEG * DEG);
    if (dot3(b, t->u) < cgate && s_q[gb] >= TRK_JUMP_RATIO * ql && !ground && !anti) {
        if (++t->jump_n >= TRK_JUMP_N) {
            memcpy(t->u, b, sizeof(b));
            t->q = s_q[gb];
            t->jump_n = 0;
        }
    } else {
        t->jump_n = 0;
    }
    return track_out(t);
}

static float goertzel_pow(const float *x, int n, float f) {
    float k = roundf(f * (float)n / FS_HZ);
    if (k < 1.0f) k = 1.0f;
    const float w = 2.0f * PI_F * k / (float)n;
    const float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static float median_small(float *v, int n) {
    for (int i = 1; i < n; i++) {
        float key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    if (n <= 0) return 0.0f;
    return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static bool is_harmonic(float f, float f0) {
    if (f0 < 50.0f || f < f0 * 1.5f) return false;
    float n = roundf(f / f0);
    if (n < 2.0f || n > 16.0f) return false;
    return fabsf(f - n * f0) / f < 0.015f;
}

neo_tone_t neo_tone(const neo_ring_t *r, uint32_t h) {
    neo_tone_t t;
    memset(&t, 0, sizeof(t));
    const uint32_t start = h - NEO_TONE_N;
    int loud = 0;
    float loud_e = -1.0f;
    for (int c = 0; c < 6; c++) {
        float e = 0.0f;
        for (uint32_t n = 0; n < NEO_TONE_N; n++) {
            float s = (float)r->s[(start + n) & r->mask][c];
            e += s * s;
        }
        if (e > loud_e) {
            loud_e = e;
            loud = c;
        }
    }
    float mean = 0.0f;
    for (uint32_t n = 0; n < NEO_TONE_N; n++) {
        s_tone_buf[n] = (float)r->s[(start + n) & r->mask][loud];
        mean += s_tone_buf[n];
    }
    mean /= (float)NEO_TONE_N;
    float acc = 0.0f;
    for (uint32_t n = 0; n < NEO_TONE_N; n++) {
        s_tone_buf[n] -= mean;
        acc += s_tone_buf[n] * s_tone_buf[n];
    }
    const float rms = sqrtf(acc / (float)NEO_TONE_N);
    t.lvl_db = 20.0f * log10f((rms + 1e-6f) / 32768.0f);
    if (rms < TONE_RMS_MIN) return t;

    static float pw[TONE_NF];
    static float fr[TONE_NF];
    int best = 0;
    for (int i = 0; i < TONE_NF; i++) {
        fr[i] = TONE_F_LO + TONE_F_STEP * (float)i;
        pw[i] = goertzel_pow(s_tone_buf, (int)NEO_TONE_N, fr[i]);
        if (pw[i] > pw[best]) best = i;
    }
    static float noise[TONE_NF];
    int nn = 0;
    for (int i = 0; i < TONE_NF; i++) {
        if (fabsf(fr[i] - fr[best]) >= 60.0f) noise[nn++] = pw[i];
    }
    const float prom = pw[best] / (median_small(noise, nn) + 1e-12f);
    t.f0 = fr[best];
    if (prom < TONE_PROM_MIN) return t;

    float vac_f = 85.0f, vac_p = 0.0f;
    for (float f = 70.0f; f <= 130.0f; f += 4.0f) {
        float p = goertzel_pow(s_tone_buf, (int)NEO_TONE_N, f);
        if (p > vac_p) {
            vac_p = p;
            vac_f = f;
        }
    }
    t.vac_f = vac_f;
    static float ctx[32];
    int nctx = 0;
    for (float f = 40.0f; f <= 250.0f && nctx < 32; f += 8.0f) {
        if (fabsf(f - vac_f) <= 20.0f) continue;
        ctx[nctx++] = goertzel_pow(s_tone_buf, (int)NEO_TONE_N, f);
    }
    const float vprom = vac_p / (median_small(ctx, nctx) + 1e-12f);
    if (prom < VAC_PROM_MAX &&
        vac_p >= VAC_RATIO_MIN * pw[best] && vprom >= VAC_PROM_MIN &&
        is_harmonic(fr[best], vac_f)) {
        t.vacuum = true;
        return t;
    }
    t.tone = true;
    return t;
}
