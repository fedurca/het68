# Detection cube — building the 6-microphone array

How to build the physical microphone array for het68, and how the cube edge
length affects direction-of-arrival (DOA) accuracy, speed and compute load.

This document describes the mechanical build; the acoustic maths matches the
estimator in [`doa.c`](doa.c). If you change the geometry here, update
`MIC_DIR` / `DOA_EDGE_M` in `doa.c` to match (see "Firmware coupling" below).

---

## 1. Geometry

The array is a **cube standing on one vertex** (body diagonal vertical), with one
microphone at the **centre of each of the six faces**, port facing outward along
the face normal. The six face-centre directions are the six vertices of an
**octahedron** — three mutually orthogonal baselines passing through the centre.

Per microphone (matching `MIC_DIR` in `doa.c`):

- **Mics 1-3** — upper three faces, elevation **+35.26°**, azimuth 0° / 120° / 240°.
- **Mics 4-6** — lower three faces, elevation **-35.26°**, azimuth interleaved by 60°
  (so each lower mic sits opposite an upper one).

Position of each mic from the array centre:

```
MIC_POS[i] = MIC_DIR[i] * (edge / 2)
```

so every microphone sits on a sphere of radius `edge/2` (the face-centre radius).

Baselines that the TDOA estimator sees:

- **Opposite faces (max baseline):** `edge`   (e.g. 512 mm)
- **Adjacent faces:** `edge / √2`             (e.g. 362 mm) — these are orthogonal directions
- **Face-centre radius:** `edge / 2`          (e.g. 256 mm)

```
            mic1 (+35.26 deg)
              \
   mic3 ------ + ------ mic2      upper ring, 120 deg apart
              /|
             / |  <- three orthogonal baselines through centre
        mic4   mic6
             \ |
              \|
            mic5 (-35.26 deg)     lower ring, opposite the upper faces
```

Why on-vertex / octahedral: three orthogonal baselines give a well-conditioned
3-D least-squares solve for azimuth **and** elevation (the `solve3()` step in
`doa.c`), with no degenerate axis and equal resolution in all directions.

---

## 2. The single number that drives everything

At `Fs = 48 kHz` and `c = 343 m/s`, one sample of inter-mic delay equals

```
delta_d = c / Fs = 343 / 48000 = 7.15 mm of path difference per sample
```

Everything below — resolution, lag search size, window length — is this 7.15 mm
quantum measured against the chosen baseline. `DOA_MAXLAG = 72` in `doa.c` exists
precisely because the 512 mm max baseline spans `512 / 7.15 = 72` samples.

---

## 3. How to build it — frame

The frame must hold the six mic positions rigidly: **7.15 mm of drift = one whole
sample of TDOA error**, so aim to keep each mic within roughly ±1 mm of nominal.
Keep the structure **open / skeletal** — a solid cube body would shadow and reflect
sound and corrupt the delays. Mount mics on thin standoffs at the geometric
face-centre points, not on panels.

### Recommended: 2020 aluminium extrusion + 3D-printed corners

- **12× 2020 aluminium extrusion rails** cut to the target edge length.
- **8× 3D-printed corner cubes** (three-way 2020 joints; PETG or ASA).
- **6× 3D-printed face-centre mic pods**, each on a light cross-brace or standoff
  reaching the centre of a face.
- Pico + Grove shield on a printed bracket at the bottom corner or the centre.

Why this is the right choice for het68: it is **rigid and dimensionally stable**,
and you can **swap only the rail length** (128 / 256 / 384 / 512 / 768 / 1024 mm)
to test every edge length in Section 5 with the *same* microphones, cabling and
firmware. That makes the edge-length comparison a controlled experiment rather
than a rebuild.

### Alternatives

- **3D-printed skeletal frame (one piece or edge struts).** Cheapest, fastest.
  Use **PETG or ASA** for outdoor UV/temperature stability — **not PLA** (it
  creeps and softens in sun, moving the mics). Good up to ~256-384 mm before
  print-bed size and flex become a problem.
- **Carbon-fibre tube + printed corner joints.** Light and very stiff; the best
  option for the large **512-1024 mm** builds where aluminium gets heavy.
- **Laser-cut panels forming closed faces.** Avoid — solid faces cause diffraction
  and shadowing. Only acceptable if perforated/open and the mic sits proud of the
  surface.

---

## 4. How to build it — microphone pods, acoustics and cabling

- **Port outward** along the face normal; nothing in front of the MEMS port.
- **Wind protection:** open-cell foam windscreen over each pod. Outdoors this is
  mandatory — wind noise dominates low frequencies and decorrelates the array.
- **Vibration isolation:** seat each ICS-43434 breakout in a **soft rubber grommet**
  so frame/motor vibration is not conducted into the mic body.
- **Strain relief:** anchor each Grove cable at the pod so tension never pulls the
  mic off its nominal point.
- **Cabling:** route the six data cables and the shared WS/SCK clock bus
  (see [`wiring_and_bom.md`](wiring_and_bom.md)) along the rails to the Pico. Keep
  the two clock daisy-chain branches (mics 1-3, mics 4-6) roughly equal length.
- **Temperature:** the speed of sound drifts ~**0.6 m/s per °C** (~0.17 %/°C). This
  is a global *scale* error on all delays, not a geometry error; compensate `c` in
  firmware from the BME280 that is already on the sensor roadmap in
  [`wiring_and_bom.md`](wiring_and_bom.md).

---

## 5. Edge length: 128 vs 256 vs 512 vs 1024 mm

All figures use `delta_d = 7.15 mm/sample`, the octahedral geometry above, and the
existing time-domain GCC estimator in `doa.c`. "Baseline" for resolution/aliasing
is the max (opposite-face) baseline = `edge`.

### 5.1 Lag search size (max integer lag = edge / 7.15 mm)

- **128 mm** → 18 samples
- **256 mm** → 36 samples
- **512 mm** → 72 samples  (current `DOA_MAXLAG`)
- **1024 mm** → 143 samples

### 5.2 Angular resolution (raw, at broadside ≈ delta_d / edge)

- **128 mm** → **3.20°**
- **256 mm** → **1.60°**
- **512 mm** → **0.80°**
- **1024 mm** → **0.40°**

Resolution **halves (improves) every time the edge doubles** — it is linear in the
baseline. The parabolic sub-sample interpolation already in `xcorr_delay()`
improves the effective delay quantum by roughly ×5-10 when SNR is good, so the
practical figures are several times finer than the raw values — but that gain is
limited by noise/coherence, not geometry, so the *relative* ranking is unchanged.

### 5.3 Spatial aliasing / grating frequency ≈ c / (2·edge)

- **128 mm** → 1340 Hz
- **256 mm** → 670 Hz
- **512 mm** → 335 Hz
- **1024 mm** → 167 Hz

This is a **soft** limit for broadband time-domain GCC: a wideband source (a drone
has energy across a broad band and many harmonics) still produces one dominant
correlation peak, because grating lobes land at different angles for different
frequencies and only the true delay reinforces across the band. It matters more
for narrowband tones — a large array leaning on a single tone above this frequency
can lock onto a grating lobe. Bigger cube ⇒ more reliance on genuine broadband
content.

### 5.4 Window length and latency (the real constraint of large arrays)

The correlation core sums over `DOA_N - 2·DOA_MAXLAG` samples. With the current
`DOA_N = 256`:

- **128 mm** (maxlag 18): 220 usable samples — comfortable.
- **256 mm** (maxlag 36): 184 usable — comfortable.
- **512 mm** (maxlag 72): 112 usable — fine, current default.
- **1024 mm** (maxlag 143): `256 - 286 < 0` — **does not fit**. Requires
  `DOA_N ≥ ~512` (e.g. 226 usable at N=512). A longer window means more latency
  and the wavefront must traverse the whole array *within* one window — a plain
  physical consequence of a bigger aperture.

### 5.5 Memory footprint

Bigger cube ⇒ longer maximum delay to search ⇒ **larger comparison window**. In
`doa.c` the window `DOA_N` auto-scales in power-of-two tiers so the usable span
stays ≥ the lag search. Only two DOA buffers depend on the edge:

- `g_work[6][DOA_N]` (float) = **24 × DOA_N bytes** — the comparison window; the
  dominant size-dependent buffer.
- `s_corr[2·DOA_MAXLAG+1]` (float) — the correlation scratch; a few hundred bytes.

Everything else is fixed: the input ring `g_ring[2048][6]` (int16) = **24576 B**
regardless of edge, plus `g_energy[6]`.

Per edge (DOA total = ring + `g_work` + `s_corr` + `g_energy`):

- **50 mm** → N=256, `g_work` 6.1 kB, DOA total ~30.1 kB
- **100 mm** → N=256, `g_work` 6.1 kB, ~30.2 kB
- **150 mm** → N=256, `g_work` 6.1 kB, ~30.2 kB
- **200 mm** → N=256, `g_work` 6.1 kB, ~30.3 kB
- **256 mm** → N=256, ~30.3 kB
- **512 mm** → N=256, ~30.6 kB
- **700 mm** → N=512, `g_work` 12.3 kB, ~36.8 kB
- **1300 mm** → N=1024, `g_work` 24.6 kB, ~49.5 kB

Key point: **memory is stepped, not smooth**. Every edge from ~50 mm up to ~600 mm
lands in the same N=256 tier, so **50 / 100 / 150 / 200 mm all cost essentially the
same RAM** (they differ only by the sub-kB `s_corr`). The window doubles only when
the edge crosses ~600 mm (→512) and ~1200 mm (→1024). Against the RP2350's 520 kB
SRAM even the 1024-sample tier is negligible. So a bigger cube *does* need a bigger
window — but the jump is discrete and only matters for the large (>600 mm) builds.

### 5.6 Compute load

MACs per window per pair = `(2·maxlag + 1) · (N − 2·maxlag)`; up to 5 pairs per
solve, at the ~5 Hz update rate (`DOA_OUT_SAMPLES = 9600`):

- **128 mm** (N=256): ~8.1 k/pair
- **256 mm** (N=256): ~13.4 k/pair
- **512 mm** (N=256): ~16.2 k/pair
- **1024 mm** (N=512): ~64.9 k/pair → ~1.6 MMAC per solve, ~1.6 MMAC/s at 5 Hz

**Conclusion: compute is not the bottleneck.** Even the 1024 mm case is a fraction
of what the RP2350 M33 FPU delivers at 150 MHz. The genuine costs of a bigger cube
are, in order: **window length / latency**, **physical size and portability**,
**frame rigidity** (holding sub-mm over a metre is hard), and **wind/turbulence
decorrelation** across a long baseline (outdoor coherence between mics drops as the
baseline grows, which *raises* the delay noise and eats into the theoretical
resolution gain).

### 5.7 Summary of the trade-off

- Bigger edge → **better angular resolution** (linear), but **lower aliasing
  frequency**, **longer window/latency**, **larger & floppier structure**, and
  **worse outdoor coherence**.
- Smaller edge → **portable, rigid, robust, low latency**, but **coarser
  resolution**.
- Range/near-field is **not** a differentiator here: the far-field (Fraunhofer)
  distance `2·edge²/λ` is only a few metres even at 1024 mm / a few kHz, and drone
  targets are far beyond that — every size sees a plane wave in practice. A single
  node gives **direction only**; range needs multi-node triangulation regardless of
  cube size.

---

## 6. Recommendation and alternative dimensions

- **Default: ~512 mm (current).** Best all-round balance: 0.80° raw / sub-0.1°
  interpolated resolution, fits the existing `DOA_N = 256` window, low latency,
  far-field for any real target. Keep it.
- **Compact / portable variant: 256 mm.** Half the resolution (1.6° raw) but half
  the size, stiffer, lower latency, easier to hand-carry and weatherproof. Good for
  a mobile or short-range node.
- **High-resolution fixed install: 1024 mm.** Only when mounted permanently and
  rigid, and only after raising `DOA_N` to ≥512. Buys 0.40° raw resolution at the
  cost of latency, bulk and wind sensitivity.
- **Avoid 128 mm** except for very compact / higher-frequency-only use: 3.2° raw is
  marginal and the 1340 Hz aliasing edge starts cutting into useful drone bandwidth
  from below-ish only if you rely on tones, but the coarse resolution is the real
  problem.

**Proposed sweet-spot band: ~350-500 mm.** If you want a single fixed size other
than 512 mm, **~384 mm** is a good compromise (≈1.07° raw resolution, maxlag ≈ 54,
comfortably inside `DOA_N = 256`, still portable). Practically, though, the
strongest recommendation is to **build the modular 2020 frame (Section 3) and
measure**: keep the mics/firmware fixed, swap rail lengths across 256 / 384 / 512 /
768 mm against a known source, and pick the smallest edge that meets your accuracy
target — resolution is only worth buying up to the point where wind coherence and
mechanical rigidity stop limiting you.

---

## 7. Firmware coupling

The geometry lives in [`doa.c`](doa.c):

- `DOA_EDGE_MM` — **the one knob**: cube edge in millimetres (integer). Positions
  scale by `edge/2`; `DOA_MAXLAG` and `DOA_N` derive from it. Default 512 mm.
- `MIC_DIR[6][3]` — the six face-centre unit directions (unchanged when you only
  scale the cube).
- `DOA_MAXLAG` — auto-derived as `ceil(edge / 7.15 mm) + 2` samples (the +2 is
  interpolation headroom); no longer a hand-set literal.
- `DOA_N` — comparison window; **auto-scales** in power-of-two tiers so the usable
  span (`DOA_N − 2·DOA_MAXLAG`) stays ≥ the lag search: 256 up to ~600 mm, 512 up
  to ~1200 mm, 1024 up to ~2400 mm. Above that the build stops with a clear
  `#error`.

Any integer edge is supported. Select it at build time without editing source:

```bash
HET68_DOA_EDGE_MM=150 ./build.sh     # 50 / 100 / 128 / 150 / 200 / 256 / 512 ...
./build.sh                            # default 512 mm
```

Rebuild per [`AGENTS.md`](AGENTS.md). The compact sizes (50-200 mm) all share the
256-sample window, so switching between them changes no buffer size (see §5.5).

---

## 8. Bill of materials — additions for the cube

Adds to the electronics BOM in [`wiring_and_bom.md`](wiring_and_bom.md).

| Item | Qty | Notes |
|---|---|---|
| 2020 aluminium extrusion rail | 12 | cut to edge length; buy spare lengths to compare sizes |
| 3D-printed corner cube (3-way 2020 joint) | 8 | PETG/ASA |
| 3D-printed face-centre mic pod + standoff | 6 | PETG/ASA; grommet seat for the breakout |
| Rubber grommet (mic isolation) | 6 | soft, decouples vibration |
| Open-cell foam windscreen | 6 | mandatory outdoors |
| Pico + Grove shield mounting bracket | 1 | printed; bottom corner or centre |
| M5 T-nuts + button-head screws | ~40 | 2020 assembly |
| Cable clips / adhesive mounts | as needed | strain relief + routing along rails |

---

## 9. Node hardware — board options

The default target is `pico2` (RP2350A, 4 MB flash, **no PSRAM**). For a networked
multi-node build, or to add on-device recording/detection, move to an RP2350B board
with flash + PSRAM + wireless. Build for it with `PICO_BOARD` (no source change):

```bash
PICO_BOARD=pimoroni_pico_plus2_w_rp2350 ./build.sh
```

Recommended and off-the-shelf candidates (all RP2350B, 16 MB flash + 8 MB PSRAM):

- **Pimoroni Pico Plus 2 W** — **recommended.** Only one with **2.4 GHz Wi-Fi +
  Bluetooth (RM2)**, which is what the multi-node plan needs. Pico footprint,
  USB-C. SDK board: `pimoroni_pico_plus2_w_rp2350`. (Non-W variant
  `pimoroni_pico_plus2_rp2350` if wireless is not needed.)
- **SparkFun Pro Micro RP2350**, **Adafruit Metro RP2350 (w/ PSRAM)**,
  **Olimex RP2350-PICO2-BB48R** — same memory, no wireless.
- **Solder Party Stamp XL / Waveshare RP2350-PiZero** — PSRAM footprint unpopulated
  (you solder your own), so not turnkey.

### PSRAM sizing — 8 MB is enough

The DOA path uses ~31 kB of the 520 kB SRAM (see §5.5), so **8 MB PSRAM is ample**
for edge detection: ~9.7 s of raw 6-channel/48 k/24-bit look-back, large FFT /
spectrogram buffers, or a 0.1-2 MB on-device classifier, many times over. The
bottleneck for detection is compute (M33 @ 150 MHz), not capacity. **A custom 16 MB
PSRAM board is not worth it** — 128 Mbit QSPI PSRAM is essentially unobtainium
(would need an RP2354B with 2× 8 MB on both chip-selects, i.e. bespoke hardware).
Note PSRAM is XIP-cached QSPI and slower than SRAM: use it for capacity (recording,
models, batch spectral work), keep the realtime correlation loop in SRAM.

### Multi-node gains (why wireless matters)

One node gives **direction only**. Two or more time-synchronised nodes give **3-D
position + range + tracking** — the big qualitative jump. Localisation accuracy is
set by the inter-node baseline (tens of metres, ~100× the cube), not the cube size;
optimal fusion of N nodes also adds up to `10·log10(N)` dB detection SNR (≈+6 dB /
~1.4-2× range at 4 nodes) plus ~N× coverage. Transport role: **Wi-Fi** carries data
and coarse sync (PTP / 802.11mc gets to ~µs); **Bluetooth** is fine for
control/telemetry but too jittery for sample-level TDOA; the on-board **acoustic
beacon** provides the fine sub-sample sync/ranging. So the transport does not
improve detection by itself — it enables fusion, and only if it carries adequate
time sync.

### Power / 1.8 V

All the boards above are **3.3 V designs** (fixed 3.3 V regulator, 3.3 V-populated
flash/PSRAM, wireless). The RP2350 core is 1.1 V and its `IOVDD`/`QSPI_IOVDD` can
run at 1.8 V (set the pad `VOLTAGE_SELECT` bit), **but `USB_OTP_VDD` needs a nominal
3.3 V for the USB full-speed PHY** — so a USB sound-card node cannot run fully on
1.8 V. A fully-1.8 V node is custom-hardware only, and only viable if it **drops
USB** (data over Wi-Fi) with 1.8 V flash and a 1.8 V PSRAM part (e.g. APS6404L
1.8 V variant). The ICS-43434 mics are 1.8 V-capable, but their IO must match the
board `IOVDD`.
