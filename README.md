# het68 — 6-channel I2S USB sound card for Raspberry Pi Pico 2 (RP2350)

This firmware turns a Raspberry Pi Pico 2 (RP2350) into a 6-channel USB audio
input device (microphone). It captures three stereo I2S data lines from six
ICS-43434 MEMS microphones (wired as three left/right pairs), moves the samples
into memory with DMA, and streams them to the host over USB Audio Class 2.0
(UAC2) using TinyUSB.

## Audio format

| Property | Value |
|---|---|
| Class | USB Audio Class 2.0 (UAC2), isochronous IN |
| Channels | 6 |
| Sample rate | 48 kHz |
| Sample format | 24-bit signed, packed little-endian (`S24_3LE`, 3 bytes/sample) |
| USB packet | 1 ms = 48 × 6 × 3 = 864 bytes (within the 1023-byte full-speed iso limit) |
| Product string | `Pico 6ch Microphone 48k/24` |

The host sees a standard 6-channel 48 kHz / 24-bit capture device and can record
all six microphones simultaneously (e.g. with `arecord`, Audacity, Reaper, OBS).

```bash
arecord -D hw:<card>,0 -c 6 -r 48000 -f S24_3LE -d 3 capture.wav
```

## How capture works

- The Pico is the I2S **master**. One PIO state machine generates the shared
  `WS`/`SCK` clocks for all six microphones.
- Three more PIO state machines (one per data line) receive the serial data.
  The RX state machines are **phase-locked** to the clock state machine: they use
  the same clock divider and are started together with
  `pio_enable_sm_mask_in_sync()`, so they sample deterministically without
  racing the external `SCK` (no `wait gpio`).
- DMA moves the captured 32-bit slots into a double-buffered memory region; the
  USB device task packs them into 24-bit `S24_3LE` frames.

## Hardware wiring (Grove Shield for Pi Pico)

Board: **Seeed Grove Shield for Pi Pico v1.0**. Set the shield power switch to
**3.3 V** (never 5 V — destroys ICS-43434 mics).

Each microphone uses **two** Grove cables: one on the shared **clock bus**, one
for **power + SD + SEL**. See `wiring_and_bom.md` for the full connector map.

| Grove port | Function | GPIO |
|---|---|---|
| **UART0** | Debug UART → Debug Probe | GP0 TX, GP1 RX |
| **UART1** | I2S clock bus — mics 1–3 (WS + SCK) | GP8 WS, GP9 SCK |
| **I2C1** | PS1240 sync beacon piezo | GP6, GP7 |
| **D16 / D18 / D20** | Mic 1–3 data (SD + SEL) | GP16–GP21 |
| **A0 / A1 / A2** | Mic 4–6 data (SD; SEL via header pigtail) | GP26–GP28, GP2–GP4 |
| **I2C0** | I2S clock bus — mics 4–6 (same GP8/GP9 as UART1) | GP8 WS, GP9 SCK |

* **Debug UART** — Grove **UART0** (or header pins 1/2/3):

    | Grove pin | GPIO | Signal | → Debug Probe "U" |
    |---|---|---|---|
    | white (2) | GP0 | UART TX | → Probe **RX** (yellow) |
    | yellow (1) | GP1 | UART RX | ← Probe **TX** (orange) |
    | black (4) | GND | ground | GND |

    Baud rate: **115200** (`./serial.sh`).

* **I2S clocks** — **UART1** (mics 1–3) and **I2C0** (mics 4–6): both connectors
  share GP9 = SCK and GP8 = WS on the shield (parallel branches, not I2C). Daisy-chain
  CLK IN → CLK OUT within each branch. Pin 3 VCC unused on clock cables.

* **Piezo** — Grove **I2C1**: passive PS1240 between GP6 and GP7 (H-bridge PWM;
  pins 3/4 unused).

* **Capture (current firmware)** — three shared SD lines on **GP2 / GP3 / GP4**
  (stereo pairs on shield headers) until the six-mono PIO update. Target per-mic
  Grove data ports are wired in `wiring_and_bom.md`.

See `wiring_and_bom.md` for mic-module layout, SEL GPIO table, and BOM.

## Drone detection (DOA + sync beacon)

Alongside the USB sound card the firmware runs an autonomous acoustic front-end:

* **Direction of arrival (DOA).** The decoded 6-channel stream is fed into a ring
  buffer and analysed continuously. A time-domain GCC (cross-correlation) over the
  six microphones estimates the time differences of arrival, which are solved by
  least squares into a 3D unit vector → **azimuth + elevation** of the loudest
  broadband source, plus a confidence and level. Results are printed to the debug
  UART, e.g.:

  ```
  DOA az=137.4 el=22.8 conf=0.7 lvl=-31.2dB ref=0 pairs=4
  ```

  - Azimuth is compass degrees (0° = north, clockwise); elevation is ±90°.
  - A single node gives **direction only**; range needs triangulation from several
    synchronised nodes.
  - The analysis runs on **core1** (full GCC loop). After OpenOCD/SWD flash,
    core1 must be launched via `het68_launch_core1()` in `core1_launch.c`
    (`multicore_reset_core1()` + 1 ms settle + verify); see `doa.c`.

* **Array geometry.** A cube standing on a vertex, **512 mm** edge. Mics 1–3 are
  the three upper faces (mic 1 = north, then +120°, +240° azimuth, all at +35.26°
  elevation); mics 4–6 are the opposite lower faces (−35.26° elevation, azimuths
  interleaved by 60°). Edit `MIC_DIR`/`DOA_EDGE_M` in `doa.c` to change it.

* **Synchronisation beacon.** The PS1240 piezo emits a periodic BPSK-modulated
  m-sequence burst on a ~4 kHz carrier (its resonance), driven differentially via
  the GP6/GP7 software H-bridge. This pseudo-noise code is what neighbouring nodes
  will cross-correlate (matched filter) for acoustic ranging / clock sync; the
  receive/ranging side is a later phase. The beacon count appears in the heartbeat
  (`bcn=`).

## Build

The Pico SDK is vendored at `./pico-sdk`; no environment variables are required.
See `BUILDING.md` for details.

### TinyUSB / pico-sdk patches

Upstream **Pico SDK 2.2.0** (commit `a1438dff`) ships **TinyUSB 0.18.0**, which
does not yet contain all fixes needed for stable RP2350 UAC2 6-channel streaming.
This repo carries local patches under [`patches/`](patches/) — full rationale and
per-fix notes are in [`PATCHES.md`](PATCHES.md).

| What | Where |
|---|---|
| Patch script | [`patches/apply_all.py`](patches/apply_all.py) |
| Detailed fix list | [`PATCHES.md`](PATCHES.md) |
| Reference `.patch` (ISO activate, PR 2937) | [`patches/tinyusb-0.18.0-pr2937-iso-activate.patch`](patches/tinyusb-0.18.0-pr2937-iso-activate.patch) |

**What they fix (summary):**

- RP2350 USB device controller quirks (`EP_ABORT` spin, NULL EP0 control register)
- `-O3` undefined behaviour from `TU_VERIFY` / `TU_ASSERT` in TinyUSB audio/stack code
- UAC2 enumeration race on Linux (`SET_INTERFACE` vs. first ISO IN packet, `err -110`)
- ISO endpoint busy-flag handling after abort

**How to apply:**

Patches are applied **automatically** on every `./build.sh` run (idempotent: reset
patched files to git HEAD inside `pico-sdk/lib/tinyusb`, then re-apply). To apply
manually:

```bash
python3 patches/apply_all.py
```

Expected output ends with `✓ All patches OK`. Do **not** edit files under
`pico-sdk/` by hand — add changes to `patches/apply_all.py` or a patch file instead.

**Target SDK version:** patches are written and tested against the vendored tree
**Pico SDK 2.2.0** / **TinyUSB 0.18.0** (`pico-sdk/lib/tinyusb`). After upgrading
the SDK submodule, re-run `apply_all.py` and check for `[pattern not found]` errors;
see `PATCHES.md` for upstream-overlap notes (some fixes may already be merged).

```bash
./build.sh
```

This produces `build/pico_6mic_soundcard.uf2` (and `.elf`). The default board is
`PICO_BOARD=pico2`.

* **Bench test without microphones** — build the diagnostic firmware, which
  bypasses I2S and streams a simulated 1 kHz tone on all channels:

```bash
HET68_USB_DIAG=ON ./build.sh
```

## Flash

* **Via Raspberry Pi Debug Probe (SWD), recommended for the lab setup:**

```bash
./fixdebugger.sh           # frees the USB bus, then flashes the ELF over SWD
./fixdebugger.sh --test    # also runs a short arecord smoke test
```

* **Via UF2 / BOOTSEL:** hold `BOOTSEL` while plugging in the Pico, then copy
  `build/pico_6mic_soundcard.uf2` to the `RP2350` mass-storage volume.

## Lab capture

10-second 6-channel reference recording (stops PipeWire so `arecord` can open the
device directly):

```bash
./record10s.sh                  # -> /tmp/lab_6ch_10s.wav
./record10s.sh capture.wav      # custom path
```

## Usage

After reset the Pico enumerates as a standard 6-channel 48 kHz / 24-bit USB audio
device on Windows, macOS and Linux. Select it as the input device in any audio
application and record from all six microphones at once.
