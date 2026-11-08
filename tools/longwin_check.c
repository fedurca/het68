// Host check: 10 s of synthetic 6ch / 48 kHz into longwin_scan().
// Build: gcc -O2 -o longwin_check tools/longwin_check.c longwin.c -lm
// Usage: ./longwin_check
#include "../longwin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RING_SZ    524288u
#define RING_MASK  (RING_SZ - 1u)
#define FS         48000.0f
#define C_SOUND    343.0f
#define EDGE_M     0.128f

static int16_t g_ring[RING_SZ][6];
static uint32_t g_head;

// Same look directions as doa.c MIC_DIR.
static const float MIC_DIR[6][3] = {
    {  0.81650f,  0.00000f,  0.57735f },
    { -0.40825f,  0.70711f,  0.57735f },
    { -0.40825f, -0.70711f,  0.57735f },
    { -0.81650f,  0.00000f, -0.57735f },
    {  0.40825f, -0.70711f, -0.57735f },
    {  0.40825f,  0.70711f, -0.57735f },
};

static uint32_t g_rng = 1u;
static int16_t irand(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (int16_t)((int32_t)(g_rng >> 16) - 32768);
}

static void clear_ring(void) {
    memset(g_ring, 0, sizeof(g_ring));
    g_head = 0;
    g_rng = 1u;
}

static void push6(const int16_t s[6]) {
    for (int c = 0; c < 6; c++) g_ring[g_head & RING_MASK][c] = s[c];
    g_head++;
}

static void delays_for(float az_deg, float el_deg, float lag[6]) {
    const float deg = (float)M_PI / 180.0f;
    float el = el_deg * deg, az = az_deg * deg;
    float u[3] = { cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el) };
    const float r = EDGE_M * 0.5f;
    for (int c = 0; c < 6; c++) {
        float d = MIC_DIR[c][0] * u[0] + MIC_DIR[c][1] * u[1] + MIC_DIR[c][2] * u[2];
        lag[c] = -d * r * FS / C_SOUND;
    }
}

static int16_t sat16(float x) {
    if (x > 30000.0f) return 30000;
    if (x < -30000.0f) return -30000;
    return (int16_t)x;
}

static void fill_drone(void) {
    float lag[6];
    delays_for(40.0f, 28.0f, lag);
    for (uint32_t n = 0; n < RING_SZ; n++) {
        int16_t s[6];
        for (int c = 0; c < 6; c++) {
            float t = ((float)n + lag[c]) / FS;
            float x = 9000.0f * sinf(2.0f * (float)M_PI * 1100.0f * t)
                    + 3500.0f * sinf(2.0f * (float)M_PI * 2200.0f * t);
            s[c] = sat16(x);
        }
        push6(s);
    }
}

static void fill_vehicle(void) {
    float y = 0.0f;
    const float a = 0.026f; // ~200 Hz
    float hist[64];
    memset(hist, 0, sizeof(hist));
    uint32_t di = 0;
    for (uint32_t n = 0; n < RING_SZ; n++) {
        float az = 10.0f + 70.0f * ((float)n / (float)RING_SZ);
        float lag[6];
        delays_for(az, -8.0f, lag);
        float u = (float)irand() * (1.0f / 32768.0f);
        y += a * (u * 22000.0f - y);
        float env = 0.55f + 0.45f * ((float)n / (float)RING_SZ);
        hist[di & 63u] = y * env;
        int16_t s[6];
        for (int c = 0; c < 6; c++) {
            int off = (int)lroundf(lag[c]);
            s[c] = sat16(hist[((int)(di & 63u) - off) & 63]);
        }
        di++;
        push6(s);
    }
}

static void fill_speech(void) {
    float y3k = 0.0f;
    const float a3 = 0.33f;
    float lag[6];
    delays_for(310.0f, 5.0f, lag);
    for (uint32_t n = 0; n < RING_SZ; n++) {
        float t = (float)n / FS;
        float syll = (sinf(2.0f * (float)M_PI * 4.4f * t) > 0.15f) ? 1.0f : 0.04f;
        float u = (float)irand() * (1.0f / 32768.0f);
        y3k += a3 * (u * 18000.0f - y3k);
        float x = syll * (0.55f * y3k
                          + 4000.0f * sinf(2.0f * (float)M_PI * 480.0f * t)
                          + 2800.0f * sinf(2.0f * (float)M_PI * 1650.0f * t)
                          + 1800.0f * sinf(2.0f * (float)M_PI * 2550.0f * t));
        int16_t s[6];
        for (int c = 0; c < 6; c++) {
            float tc = t + lag[c] / FS;
            float xc = syll * (0.55f * y3k
                               + 4000.0f * sinf(2.0f * (float)M_PI * 480.0f * tc)
                               + 2800.0f * sinf(2.0f * (float)M_PI * 1650.0f * tc)
                               + 1800.0f * sinf(2.0f * (float)M_PI * 2550.0f * tc));
            (void)x;
            s[c] = sat16(xc);
        }
        push6(s);
    }
}

static int run_scene(const char *name, void (*fill)(void), longwin_class_t want) {
    clear_ring();
    fill();
    neo_ring_t r = { (const volatile int16_t (*)[6])g_ring, RING_MASK };
    longwin_t s;
    longwin_scan(&r, g_head, RING_SZ, MIC_DIR, &s);
    longwin_class_t got = longwin_pick(&s);
    const char *gn =
        got == LW_DRONE ? "drone" :
        got == LW_VEHICLE ? "vehicle" :
        got == LW_SPEECH ? "speech" : "none";
    printf("%-8s got=%-8s drone=%.2f veh=%.2f speech=%.2f az=%.1f daz=%.1f "
           "duty=%.2f low=%.2f mid=%.2f sp=%.2f syll=%.2f loud=%u\n",
           name, gn, s.s_drone, s.s_vehicle, s.s_speech, s.az, s.daz_deg,
           s.duty, s.low_r, s.mid_r, s.sp_r, s.mod_syll, s.loud);
    if (got != want) {
        fprintf(stderr, "FAIL %s: expected %d got %d\n", name, (int)want, (int)got);
        return 1;
    }
    return 0;
}

static int run_silence(void) {
    clear_ring();
    for (uint32_t n = 0; n < RING_SZ; n++) {
        int16_t s[6] = { 12, -7, 4, -3, 9, -5 };
        push6(s);
    }
    neo_ring_t r = { (const volatile int16_t (*)[6])g_ring, RING_MASK };
    longwin_t s;
    longwin_scan(&r, g_head, RING_SZ, MIC_DIR, &s);
    longwin_class_t got = longwin_pick(&s);
    printf("silence  got=%s drone=%.2f veh=%.2f speech=%.2f loud=%u\n",
           got == LW_NONE ? "none" : "?", s.s_drone, s.s_vehicle, s.s_speech, s.loud);
    if (got != LW_NONE) {
        fprintf(stderr, "FAIL silence: expected none\n");
        return 1;
    }
    return 0;
}

int main(void) {
    int nfail = 0;
    nfail += run_scene("drone",   fill_drone,   LW_DRONE);
    nfail += run_scene("vehicle", fill_vehicle, LW_VEHICLE);
    nfail += run_scene("speech",  fill_speech,  LW_SPEECH);
    nfail += run_silence();
    if (nfail) {
        fprintf(stderr, "%d scene(s) failed\n", nfail);
        return 1;
    }
    printf("longwin_check: 4/4 ok\n");
    return 0;
}
