// longwin.h — 10 s ring features + drone / vehicle / speech scores.
//
// Plain C, no SDK. core1 and tools/longwin_check.c share this.
#pragma once

#include "neo_srp.h"

#include <stdint.h>

#define LW_FS_HZ     48000u
#define LW_HOP       2048u    // 42.7 ms
#define LW_STRIDE    4u       // analyse every 4th sample (12 kHz)
#define LW_MAX_HOPS  256u     // 256 × 2048 = 524288 = full PSRAM ring

typedef enum {
    LW_NONE    = 0,
    LW_DRONE   = 1,
    LW_VEHICLE = 2,
    LW_SPEECH  = 3
} longwin_class_t;

typedef struct {
    float s_drone;
    float s_vehicle;
    float s_speech;
    float az;         // energy-weighted bearing of loud hops, deg
    float el;
    float conf;
    float lvl_db;
    float daz_deg;    // azimuth span of loud hops (0..180)
    float az_jitter;  // median hop-to-hop |Δaz| of loud hops, deg
    float duty;       // fraction of hops above the noise floor
    float low_r;      // <~250 Hz
    float mid_r;      // ~300–1400 Hz (blade / engine mid)
    float sp_r;       // ~300–3400 Hz (speech)
    float high_r;     // >~3400 Hz
    float mod_syll;   // 3–8 Hz envelope fraction (syllabic)
    float mod_slow;   // ~1 Hz envelope fraction (pass-by / rumble)
    float crest;
    uint32_t hops;
    uint32_t loud;
} longwin_t;

// Scan the last `nframes` samples ending at `head` (nframes ≤ ring length).
// mic_dir[6][3] are unit look directions (same as firmware MIC_DIR).
void longwin_scan(const neo_ring_t *r, uint32_t head, uint32_t nframes,
                  const float mic_dir[6][3], longwin_t *out);

// Winner from the three scores. Ambiguous or quiet scenes return LW_NONE.
longwin_class_t longwin_pick(const longwin_t *s);
