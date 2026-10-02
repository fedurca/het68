// Host check: run neo_tone() on a 6ch / 48 kHz / S16 WAV (same gate as core1).
// Build:  gcc -O2 -o neo_wav_check tools/neo_wav_check.c neo_srp.c -lm
// Usage:  ./neo_wav_check flight.wav
#include "../neo_srp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
} wav_hdr_t;
#pragma pack(pop)

#define RING_BITS 16u
#define RING_SZ   (1u << RING_BITS)
#define RING_MASK (RING_SZ - 1u)

static int16_t g_ring[RING_SZ][6];
static uint32_t g_head;

static int load_wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    wav_hdr_t h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return -1; }
    if (memcmp(h.riff, "RIFF", 4) || memcmp(h.wave, "WAVE", 4)) {
        fprintf(stderr, "not a WAV\n"); fclose(f); return -1;
    }
    // Skip to data chunk (fmt may be followed by extras).
    char id[4];
    uint32_t csz;
    for (;;) {
        if (fread(id, 1, 4, f) != 4 || fread(&csz, 4, 1, f) != 1) {
            fprintf(stderr, "no data chunk\n"); fclose(f); return -1;
        }
        if (memcmp(id, "data", 4) == 0) break;
        if (fseek(f, (long)csz, SEEK_CUR) != 0) { fclose(f); return -1; }
    }
    if (h.channels != 6 || h.sample_rate != 48000 || h.bits != 16) {
        fprintf(stderr, "need 6ch / 48 kHz / S16 (got %uch %u Hz %ubit)\n",
                h.channels, h.sample_rate, h.bits);
        fclose(f);
        return -1;
    }
    uint32_t nframes = csz / (6u * 2u);
    printf("wav frames=%u dur=%.2fs\n", nframes, nframes / 48000.0);
    int16_t frame[6];
    uint32_t nz[6] = {0};
    for (uint32_t i = 0; i < nframes; i++) {
        if (fread(frame, 2, 6, f) != 6) break;
        for (int c = 0; c < 6; c++) {
            g_ring[g_head & RING_MASK][c] = frame[c];
            if (frame[c]) nz[c]++;
        }
        g_head++;
    }
    fclose(f);
    printf("nonzero samples:");
    for (int c = 0; c < 6; c++) printf(" ch%d=%u", c, nz[c]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.wav\n", argv[0]);
        return 2;
    }
    if (load_wav(argv[1]) != 0) return 1;

    float mic[6][3] = {
        { 0.0523f, 0, 0.0370f }, { -0.0261f, 0.0453f, 0.0370f },
        { -0.0261f, -0.0453f, 0.0370f }, { -0.0523f, 0, -0.0370f },
        { 0.0261f, -0.0453f, -0.0370f }, { 0.0261f, 0.0453f, -0.0370f },
    };
    neo_srp_init(mic);
    neo_ring_t ring = { (const volatile int16_t (*)[6])g_ring, RING_MASK };

    uint32_t tone_n = 0, vac_n = 0, ticks = 0, hold = 0, detect_n = 0;
    for (uint32_t h = NEO_TONE_N; h < g_head; h += 9600u) {
        ticks++;
        neo_tone_t t = neo_tone(&ring, h);
        if (t.vacuum) vac_n++;
        if (t.tone) {
            tone_n++;
            hold++;
        } else if (hold > 0u) {
            hold--;
        }
        // SRP needs ≥2 live mics; on a 1-ch capture the tone-hold fallback is
        // what firmware 1.21.0 uses (see analyse_drone in doa.c).
        int report = (hold >= 3u);
        if (report) detect_n++;
        printf("t=%5.2fs tone=%d vac=%d f0=%5.0f hold=%u -> %s\n",
               h / 48000.0, t.tone, t.vacuum, t.f0, hold,
               report ? "DRONE" : "-");
    }
    printf("summary: ticks=%u tone=%u vac=%u detect=%u (%.0f%%)\n",
           ticks, tone_n, vac_n, detect_n,
           ticks ? 100.0 * detect_n / ticks : 0.0);
    return detect_n > 0 ? 0 : 1;
}
