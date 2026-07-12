# het68 wiring on RP2350 (Pico 2) — Grove Shield

6-channel USB sound card, 48 kHz / 24-bit (`S24_3LE`), six ICS-43434 microphones
on a **Seeed Grove Shield for Pi Pico v1.0** (SKU 103100142).

**Shield setup:** power switch → **3.3 V** (never 5 V — destroys the mics).

## Grove connector map

| Connector | Use | GPIO | Notes |
|---|---|---|---|
| **UART0** | Debug UART → Debug Probe | GP0 TX, GP1 RX | |
| **UART1** | I2S clock bus (daisy-chain) | GP9 SCK, GP8 WS | pin 3 (VCC) unused |
| **I2C1** | PS1240 piezo (H-bridge) | GP6, GP7 | pins 3/4 unused |
| **D16** | Mic 1 data cable | GP16 SD, GP17 SEL | |
| **D18** | Mic 2 data cable | GP18 SD, GP19 SEL | |
| **D20** | Mic 3 data cable | GP20 SD, GP21 SEL | |
| **A0** | Mic 4 data cable | GP26 SD | SEL GP27 via pigtail → header |
| **A1** | Mic 5 data cable | GP28 SD | SEL GP2 via pigtail → header |
| **A2** | Mic 6 data cable | GP3 SD | SEL GP4 via pigtail → header |
| **I2C0** | *do not use* | GP8/GP9 | same pins as UART1 |

Standard Grove cable colours: **1 yellow, 2 white, 3 red, 4 black**.

---

## Debug UART — connector **UART0** (GP0/GP1)

Independent of I2S. Grove cable into **UART0**, or direct wires to header pins 1/2/3.

| Grove pin | Colour | Signal | GPIO | → Debug Probe "U" |
|---|---|---|---|---|
| 1 | yellow | UART **RX** | GP1 | ← Probe **TX** (orange) |
| 2 | white | UART **TX** | GP0 | → Probe **RX** (yellow) |
| 3 | red | NC | — | |
| 4 | black | GND | GND | GND |

```bash
./serial.sh    # 115200 baud
```

---

## I2S clock bus — connector **UART1** (GP8/GP9)

Shared **WS** and **SCK** for all six microphones. One Grove cable from **UART1**,
then daisy-chained through each mic module (CLK IN → CLK OUT). **No power** on
this bus — pin 3 is not used.

| Grove pin | Colour | Signal | GPIO |
|---|---|---|---|
| 1 | yellow | **SCK** | GP9 |
| 2 | white | **WS** | GP8 |
| 3 | red | NC | — |
| 4 | black | GND | GND |

Chain: `Shield UART1 → Mic1 CLK → Mic2 CLK → … → Mic6 CLK`.

---

## Mic module (×6)

Each module carries one ICS-43434, two Grove sockets, and optional pass-through
for the clock bus.

```
[CLK IN] ──► ICS-43434 ──► [CLK OUT]     WS, SCK, GND pass through
                │
           [DATA] ─────────────► shield (SD, SEL, 3V3, GND)
```

| ICS-43434 pin | Source |
|---|---|
| VDD | red (3V3) on **data** cable |
| GND | black on **data** cable |
| WS, SCK | **clock** cable |
| SD | yellow on **data** cable |
| SEL | white on **data** cable → GPIO (set once at boot) |

**SEL (L/R)** is driven by the Pico (not hardwired to GND/3V3). Example mapping:

| Mic | USB ch | SEL GPIO | `sel_right` |
|---|---|---|---|
| 1 | 1 | GP17 | 0 (L) |
| 2 | 2 | GP19 | 1 (R) |
| 3 | 3 | GP21 | 0 |
| 4 | 4 | GP27 | 1 |
| 5 | 5 | GP2 | 0 |
| 6 | 6 | GP4 | 1 |

Breakout PCB: https://www.aliexpress.com/item/1005008956861273.html

### Data cables — connector assignment

| Mic | Shield connector | SD (pin 1) | SEL (pin 2) | 3V3 / GND |
|---|---|---|---|---|
| 1 | **D16** | GP16 | GP17 | pins 3/4 |
| 2 | **D18** | GP18 | GP19 | pins 3/4 |
| 3 | **D20** | GP20 | GP21 | pins 3/4 |
| 4 | **A0** + pigtail | GP26 | GP27 on header | pins 3/4 |
| 5 | **A1** + pigtail | GP28 | GP2 on header | pins 3/4 |
| 6 | **A2** + pigtail | GP3 | GP4 on header | pins 3/4 |

Analog ports A0–A2 only route **pin 1** to the GPIO; run the white SEL wire
from pins 4–6 as a short pigtail to the header pin listed above.

### Firmware capture (current)

PIO capture still uses **three shared SD lines** (stereo pairs) on **GP2 / GP3 /
GP4** until the six-mono PIO update lands. For bring-up with the current
firmware, wire pairs 1+2 → GP2, 3+4 → GP3, 5+6 → GP4 on the shield **female
headers**, with SEL hardwired (odd mics → GND, even → 3V3) as before.

---

## Sync beacon piezo — connector **I2C1** (GP6/GP7)

One passive **PS1240** between the two signal pins. Do **not** use pin 3 (VCC)
or pin 4 (GND) on this Grove cable.

| Grove pin | Colour | Signal | GPIO |
|---|---|---|---|
| 1 | yellow | Piezo B | GP7 |
| 2 | white | Piezo A | GP6 |
| 3 | red | NC | — |
| 4 | black | NC | — |

Software H-bridge: one PWM slice, channel B inverted → ±VDD across the element.
No series resistor; keep leads short.

---

## Planned peripherals (I2C on headers)

Future sensors on free header pins (e.g. GP2/GP3 as I2C1 after the six-mono
mic migration):

- BME280 — temperature, pressure, humidity
- INA219 — single-channel current/voltage
- INA3221 — three-channel current/voltage

---

## Bill of materials

| Item | Qty |
|---|---|
| Raspberry Pi Pico 2 (RP2350) | 1 |
| Grove Shield for Pi Pico v1.0 | 1 |
| ICS-43434 breakout / mic module | 6 |
| Grove cable 4-pin | 7+ (1 clock chain + 6 data) |
| PS1240 passive piezo | 1 |
| Raspberry Pi Debug Probe | 1 |
