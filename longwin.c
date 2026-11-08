// longwin.c — see longwin.h.
#include "longwin.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// One-pole coefficients at 12 kHz (stride-4 of 48 kHz):
// a = 1 - exp(-2π f / 12000)
#define LW_A250   0.1227f
#define LW_A300   0.1453f
#define LW_A1400  0.5195f
#define LW_A3400  0.8310f

#define LW_RMS_FLOOR 40.0f

static float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

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
    if (x <= a || x >= d) return 0.0f;
    if (x < b) return (x - a) / (b - a + 1e-6f);
    if (x > c) return (d - x) / (d - c + 1e-6f);
    return 1.0f;
}

static float ang_diff_deg(float a, float b) {
    float d = fabsf(a - b);
    return (d > 180.0f) ? (360.0f - d) : d;
}

static void sort256(float *v, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        float key = v[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

static float goertzel_n(const float *x, int n, float fs, float f) {
    float k = roundf(f * (float)n / fs);
    if (k < 1.0f) k = 1.0f;
    const float w = 2.0f * (float)M_PI * k / (float)n;
    const float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

void longwin_scan(const neo_ring_t *r, uint32_t head, uint32_t nframes,
                  const float mic_dir[6][3], longwin_t *out) {
    memset(out, 0, sizeof(*out));
    if (!r || !r->s || nframes < 8u * LW_HOP) return;

    if (nframes > (LW_MAX_HOPS * LW_HOP)) nframes = LW_MAX_HOPS * LW_HOP;
    nframes = (nframes / LW_HOP) * LW_HOP;
    const uint32_t nh = nframes / LW_HOP;
    if (nh < 8u || nh > LW_MAX_HOPS) return;

    const uint32_t start0 = head - nframes;
    const uint32_t mask = r->mask;
    const uint32_t ns = LW_HOP / LW_STRIDE;

    static float hop_rms[LW_MAX_HOPS];
    static float hop_az[LW_MAX_HOPS];
    static float hop_el[LW_MAX_HOPS];
    static float hop_peak[LW_MAX_HOPS];
    static float hop_low[LW_MAX_HOPS];
    static float hop_mid[LW_MAX_HOPS];
    static float hop_sp[LW_MAX_HOPS];
    static float hop_hi[LW_MAX_HOPS];
    static float hop_all[LW_MAX_HOPS];

    for (uint32_t h = 0; h < nh; h++) {
        const uint32_t hop0 = start0 + h * LW_HOP;
        float e_mic[6] = {0, 0, 0, 0, 0, 0};
        float y250 = 0.0f, y300 = 0.0f, y1400 = 0.0f, y3400 = 0.0f;
        float e_low = 0.0f, e_mid = 0.0f, e_sp = 0.0f, e_hi = 0.0f, e_all = 0.0f;
        float peak = 0.0f;
        for (uint32_t n = 0; n < LW_HOP; n += LW_STRIDE) {
            const volatile int16_t *sl = r->s[(hop0 + n) & mask];
            int32_t mix_i = 0;
            for (int c = 0; c < 6; c++) {
                float x = (float)sl[c];
                mix_i += sl[c];
                e_mic[c] += x * x;
                float ax = x < 0.0f ? -x : x;
                if (ax > peak) peak = ax;
            }
            float mix = (float)mix_i * (1.0f / 6.0f);
            y250  += LW_A250  * (mix - y250);
            y300  += LW_A300  * (mix - y300);
            y1400 += LW_A1400 * (mix - y1400);
            y3400 += LW_A3400 * (mix - y3400);
            float mid = y1400 - y300;
            float sp  = y3400 - y300;
            float hi  = mix - y3400;
            e_low += y250 * y250;
            e_mid += mid * mid;
            e_sp  += sp * sp;
            e_hi  += hi * hi;
            e_all += mix * mix;
        }
        float et = e_mic[0]+e_mic[1]+e_mic[2]+e_mic[3]+e_mic[4]+e_mic[5] + 1e-6f;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        for (int c = 0; c < 6; c++) {
            float w = e_mic[c] / et;
            wx += mic_dir[c][0] * w;
            wy += mic_dir[c][1] * w;
            wz += mic_dir[c][2] * w;
        }
        float nrm = sqrtf(wx * wx + wy * wy + wz * wz) + 1e-9f;
        float az = atan2f(wy / nrm, wx / nrm) * (180.0f / (float)M_PI);
        if (az < 0.0f) az += 360.0f;
        float el = asinf(clampf(wz / nrm, -1.0f, 1.0f)) * (180.0f / (float)M_PI);

        hop_rms[h]  = sqrtf(e_all / (float)ns);
        hop_peak[h] = peak;
        hop_az[h]   = az;
        hop_el[h]   = el;
        hop_low[h]  = e_low;
        hop_mid[h]  = e_mid;
        hop_sp[h]   = e_sp;
        hop_hi[h]   = e_hi;
        hop_all[h]  = e_all;
    }

    float sorted[LW_MAX_HOPS];
    memcpy(sorted, hop_rms, nh * sizeof(float));
    sort256(sorted, nh);
    const float mx = sorted[nh - 1u];
    // Relative to the loudest hop so a steady drone/vehicle (flat RMS) still
    // counts; absolute floor rejects a quiet ring. Speech troughs (~few % of
    // peak) fall below 0.25×max and become the off-duty gaps.
    const float thr = fmaxf(LW_RMS_FLOOR, 0.25f * mx);

    float e_low = 0.0f, e_mid = 0.0f, e_sp = 0.0f, e_hi = 0.0f, e_all = 0.0f;
    float peak_sum = 0.0f, rms_sum = 0.0f;
    float csin = 0.0f, ccos = 0.0f, esin = 0.0f;
    uint32_t loud = 0;
    for (uint32_t h = 0; h < nh; h++) {
        if (hop_rms[h] < thr) continue;
        loud++;
        e_low += hop_low[h];
        e_mid += hop_mid[h];
        e_sp  += hop_sp[h];
        e_hi  += hop_hi[h];
        e_all += hop_all[h];
        peak_sum += hop_peak[h];
        rms_sum  += hop_rms[h];
        float ar = hop_az[h] * ((float)M_PI / 180.0f);
        csin += sinf(ar);
        ccos += cosf(ar);
        esin += hop_el[h];
    }

    out->hops = nh;
    out->loud = loud;
    if (loud == 0u) return;

    const float den = e_all + 1e-6f;
    out->low_r  = e_low / den;
    out->mid_r  = e_mid / den;
    out->sp_r   = e_sp  / den;
    out->high_r = e_hi  / den;
    out->duty   = (float)loud / (float)nh;
    float mean_rms = rms_sum / (float)loud;
    out->lvl_db = 20.0f * log10f((mean_rms + 1e-6f) / 32768.0f);
    out->crest  = (peak_sum / (float)loud) / (mean_rms + 1e-6f);

    float nxy = sqrtf(csin * csin + ccos * ccos) + 1e-9f;
    float azm = atan2f(csin, ccos) * (180.0f / (float)M_PI);
    if (azm < 0.0f) azm += 360.0f;
    out->az = azm;
    out->el = esin / (float)loud;
    // R = 1: all loud hops share one bearing. A talker plus a gain-bias
    // ghost, or a far-field equal-level tone, spreads R and inflates span.
    float R = nxy / (float)loud;
    float devs[LW_MAX_HOPS];
    float jits[LW_MAX_HOPS];
    uint32_t nd = 0, nj = 0;
    int32_t prev = -1;
    for (uint32_t h = 0; h < nh; h++) {
        if (hop_rms[h] < thr) continue;
        devs[nd++] = ang_diff_deg(hop_az[h], azm);
        if (prev >= 0) {
            jits[nj++] = ang_diff_deg(hop_az[h], hop_az[(uint32_t)prev]);
        }
        prev = (int32_t)h;
    }
    sort256(devs, nd);
    float d90 = (nd > 0u) ? devs[(nd * 9u) / 10u] : 0.0f;
    float span = 2.0f * d90;
    if (span > 180.0f) span = 180.0f;
    out->daz_deg = span;
    if (nj > 0u) {
        sort256(jits, nj);
        out->az_jitter = jits[nj / 2u];
    }
    out->conf = clampf(0.35f * out->duty + 0.65f * R, 0.0f, 1.0f);

    // Envelope modulation on the hop-RMS series (fs ≈ 23.4 Hz).
    float env[LW_MAX_HOPS];
    float emean = 0.0f;
    for (uint32_t h = 0; h < nh; h++) emean += hop_rms[h];
    emean /= (float)nh;
    for (uint32_t h = 0; h < nh; h++) env[h] = hop_rms[h] - emean;
    const float efs = (float)LW_FS_HZ / (float)LW_HOP;
    float p1 = goertzel_n(env, (int)nh, efs, 1.0f);
    float p4 = goertzel_n(env, (int)nh, efs, 4.2f);
    float p6 = goertzel_n(env, (int)nh, efs, 6.0f);
    float e2 = 0.0f;
    for (uint32_t h = 0; h < nh; h++) e2 += env[h] * env[h];
    float ptot = p1 + p4 + p6 + 0.15f * e2 + 1e-6f;
    out->mod_slow = p1 / ptot;
    out->mod_syll = (p4 + p6) / ptot;

    out->s_drone =
        1.7f * score_above(out->mid_r, 0.16f, 0.38f) +
        1.3f * score_below(out->mod_syll, 0.07f, 0.22f) +
        1.0f * score_above(out->duty, 0.22f, 0.65f) +
        0.7f * score_below(out->daz_deg, 10.0f, 28.0f) +
        0.6f * score_below(out->low_r, 0.14f, 0.34f);

    // Pass-by: moderate span AND smooth hop-to-hop az. A talker plus a
    // gain-bias ghost has daz near 180° and must not look like a car.
    float passby =
        score_band(out->daz_deg, 12.0f, 22.0f, 75.0f, 120.0f) *
        score_below(out->az_jitter, 6.0f, 20.0f);
    float idle =
        score_above(out->duty, 0.45f, 0.80f) *
        score_above(out->low_r, 0.24f, 0.46f) *
        score_below(out->daz_deg, 8.0f, 28.0f);

    out->s_vehicle =
        1.4f * score_above(out->low_r, 0.22f, 0.44f) +
        1.1f * score_above(out->duty, 0.32f, 0.78f) +
        1.8f * passby +
        1.2f * idle +
        1.0f * score_below(out->mod_syll, 0.08f, 0.22f) +
        0.5f * score_below(out->crest, 2.4f, 4.0f);

    out->s_speech =
        2.0f * score_above(out->mod_syll, 0.11f, 0.32f) +
        1.4f * score_above(out->sp_r, 0.18f, 0.42f) +
        1.1f * score_band(out->duty, 0.10f, 0.22f, 0.62f, 0.88f) +
        0.7f * score_above(out->crest, 1.9f, 3.4f) +
        0.8f * score_below(out->low_r, 0.10f, 0.28f);
}

longwin_class_t longwin_pick(const longwin_t *s) {
    if (!s || s->loud < 6u) return LW_NONE;
    float a = s->s_drone, b = s->s_vehicle, c = s->s_speech;
    // Syllabic mid-band energy is speech, even with room rumble under it.
    const bool speechy = (s->mod_syll >= 0.12f && s->sp_r >= 0.22f);
    if (speechy) b *= 0.22f;
    // Floor / hotter bottom mics pull energy-az below the horizon.
    if (s->el < -28.0f) b *= 0.25f;

    float m = a;
    longwin_class_t w = LW_DRONE;
    if (b > m) { m = b; w = LW_VEHICLE; }
    if (c > m) { m = c; w = LW_SPEECH; }
    float second = 0.0f;
    if (w != LW_DRONE   && a > second) second = a;
    if (w != LW_VEHICLE && b > second) second = b;
    if (w != LW_SPEECH  && c > second) second = c;
    if (m < 1.55f) return LW_NONE;
    if (m < second + 0.32f) return LW_NONE;
    if (w == LW_VEHICLE && c >= 1.15f && b < c + 0.85f) return LW_SPEECH;
    if (w == LW_VEHICLE && s->duty < 0.24f && s->daz_deg < 16.0f) return LW_NONE;
    if (w == LW_VEHICLE && s->el < -30.0f) return LW_NONE;
    return w;
}
