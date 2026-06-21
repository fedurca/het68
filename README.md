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

## Hardware wiring

> **WARNING:** Power the microphones from **3V3 (OUT), Pin 36** — **not** VSYS,
> or you will destroy them.

* **Power**
    * `VDD` of all 6 mics → **3V3 (OUT)**, Pico Pin 36
    * `GND` of all 6 mics → `GND`, Pico Pin 38

* **Shared clocks (Pico is master)**
    * `GP0` → `WS` (word select / LRCLK) on all 6 mics
    * `GP1` → `SCK` (serial / bit clock) on all 6 mics

* **Data lines (3 stereo pairs)**
    * **Channels 1 & 2 (GP2):** mic 1 `SEL`→`GND` (left), mic 2 `SEL`→`3V3` (right), joined `SD` → `GP2`
    * **Channels 3 & 4 (GP3):** mic 3 `SEL`→`GND` (left), mic 4 `SEL`→`3V3` (right), joined `SD` → `GP3`
    * **Channels 5 & 6 (GP4):** mic 5 `SEL`→`GND` (left), mic 6 `SEL`→`3V3` (right), joined `SD` → `GP4`

* **Debug UART (Raspberry Pi Debug Probe, independent of I2S)**

    Debug Probe cable (connector "U"), three wires:

    | Wire | Pico pin | GPIO | Signal | → Debug Probe "U" |
    |---|---|---|---|---|
    | **yellow** | Pin 11 | GP8 | UART TX | → Probe **RX** |
    | **orange** | Pin 12 | GP9 | UART RX | ← Probe **TX** |
    | **black** | Pin 13 | GND | ground | GND |

    * Baud rate: **115200** (`./serial.sh`)
    * Note: GP8/GP9 are **UART1**; I2S runs on GP0–GP4 with no conflict.

See `wiring_and_bom.md` for the full pinout and bill of materials.

## Build

The Pico SDK is vendored at `./pico-sdk`; no environment variables are required.
See `BUILDING.md` for details.

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

## Usage

After reset the Pico enumerates as a standard 6-channel 48 kHz / 24-bit USB audio
device on Windows, macOS and Linux. Select it as the input device in any audio
application and record from all six microphones at once.
