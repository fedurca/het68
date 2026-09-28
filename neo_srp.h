// neo_srp.h — DJI Neo 2 detector: blade-tone gate + wideband SRP-PHAT tracker.
//
// Plain C with no SDK calls, so a host build runs the same code on recorded
// flights as core1 runs on the cube.
#ifndef NEO_SRP_H
#define NEO_SRP_H

#include <stdbool.h>
#include <stdint.h>

#define NEO_SRP_N    1024u   // SRP frame (21.3 ms at 48 kHz)
#define NEO_SRP_HOP  512u
#define NEO_TONE_N   2048u   // tone-gate window

typedef struct {
    const volatile int16_t (*s)[6];
    uint32_t mask;
} neo_ring_t;

typedef struct {
    bool tone;       // sharp blade line in 0.9–1.36 kHz
    bool vacuum;     // that line is only a harmonic of a 70–130 Hz tone
    float f0;        // blade line (Hz)
    float vac_f;     // vacuum fundamental (Hz)
    float lvl_db;    // loudest mic (dBFS)
} neo_tone_t;

typedef struct {
    bool active;
    float az;        // degrees, 0 = mic1 (north), clockwise toward east
    float el;        // degrees above the cube centre
    float q;         // SRP coherence of the tracked peak, 0..1
} neo_track_t;

// mic_pos: face-centre positions in metres, cube centre at the origin.
void neo_srp_init(const float mic_pos[6][3]);
// Add one NEO_SRP_N frame that starts at sample index `start`.
void neo_srp_frame(const neo_ring_t *r, uint32_t start);
// Blade-tone gate on the NEO_TONE_N samples that end at `h`.
neo_tone_t neo_tone(const neo_ring_t *r, uint32_t h);
// Evaluate the direction map and step the tracker (call every 0.2 s).
neo_track_t neo_track_update(bool tone, float c_sound_m_s);

#endif
