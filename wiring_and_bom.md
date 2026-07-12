# het68 wiring on RP2350 (Pico 2) — Grove Shield

6-channel USB sound card, 48 kHz / 24-bit (`S24_3LE`), six ICS-43434 microphones
on a **Seeed Grove Shield for Pi Pico v1.0** (SKU 103100142).

**Shield setup:** power switch → **3.3 V** (never 5 V — destroys the mics).

## Grove connector map

| Connector | Use | GPIO | Notes |
|---|---|---|---|
| **UART0** | Debug UART → Debug Probe | GP0 TX, GP1 RX | |
| **UART1** | I2S clock bus — mics 1–3 | GP9 SCK, GP8 WS | pin 3 (VCC) unused |
| **I2C0** | I2S clock bus — mics 4–6 | GP9 SCK, GP8 WS | same pins as UART1; not I2C |
| **I2C1** | PS1240 piezo (H-bridge) | GP6, GP7 | pins 3/4 unused |
| **D16** | Mic 1 data | GP16 SD | SEL → GND on module |
| **D18** | Mic 2 data | GP18 SD | SEL → GND on module |
| **D20** | Mic 3 data | GP20 SD | SEL → GND on module |
| **A0** | Mic 4 data | GP26 SD (pin 1) | pin 2 = GP27 (shared with A1 pin 1) |
| **A1** | Mic 5 data | GP27 SD (pin 1) | pin 2 = GP28 (shared with A2 pin 1) |
| **A2** | Mic 6 data | GP28 SD (pin 1) | pin 2 unused |

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

## I2S clock bus — **UART1** + **I2C0** (GP8/GP9)

Shared **WS** and **SCK** for all six microphones. Firmware drives **GP8** and
**GP9** once; connectors **UART1** and **I2C0** are wired in parallel on the
shield. Use both as two branches of one clock bus.

**No power** on the clock cables — pin 3 is not used.

| Grove pin | Colour | Signal | GPIO |
|---|---|---|---|
| 1 | yellow | **SCK** | GP9 |
| 2 | white | **WS** | GP8 |
| 3 | red | NC | — |
| 4 | black | GND | GND |

```
                    Pico (GP8 WS, GP9 SCK)
                           │
                    ┌──────┴──────┐
                    │   Shield    │
                 [UART1]      [I2C0]
                    │              │
              Mic1→Mic2→Mic3   Mic4→Mic5→Mic6
```

| Branch | Shield connector | Clock chain |
|---|---|---|
| Mics 1–3 | **UART1** | `UART1 → Mic1 CLK → Mic2 CLK → Mic3 CLK` |
| Mics 4–6 | **I2C0** | `I2C0 → Mic4 CLK → Mic5 CLK → Mic6 CLK` |

---

## Mic module (×6)

Each module carries one ICS-43434, two Grove sockets, and pass-through for the
clock bus.

```
[CLK IN] ──► ICS-43434 ──► [CLK OUT]     WS, SCK, GND pass through
                │
           [DATA] ─────────────► shield (SD, 3V3, GND)
```

| ICS-43434 pin | Source |
|---|---|
| VDD | red (3V3) on **data** cable |
| GND | black on **data** cable |
| WS, SCK | **clock** cable |
| SD | yellow on **data** cable (pin 1) |
| SEL | **hardwired to GND** on the module (left channel only) |

**SEL:** strap **SEL → GND** on every breakout (jumper or solder bridge). The
white Grove wire (pin 2) on the data cable is **not used**. Firmware captures
only the left I2S slot per mic.

### Analog ports A0–A2 — shield chaining

The Grove Shield chains secondary pins between adjacent analog connectors:

| GPIO | Shield routing |
|---|---|
| **GP26** | A0 pin 1 only |
| **GP27** | A0 pin 2 = A1 pin 1 (same net) |
| **GP28** | A1 pin 2 = A2 pin 1 (same net) |

Wire **one SD line per mic on pin 1 (yellow)** of each port. Do not use pin 2
for independent signals — mic 4 uses only A0, mic 5 only A1, mic 6 only A2.

### Data cables — connector assignment

| Mic | USB ch | Shield | SD (pin 1) | SEL | 3V3 / GND |
|---|---|---|---|---|---|
| 1 | 1 | **D16** | GP16 | GND on module | pins 3/4 |
| 2 | 2 | **D18** | GP18 | GND on module | pins 3/4 |
| 3 | 3 | **D20** | GP20 | GND on module | pins 3/4 |
| 4 | 4 | **A0** | GP26 | GND on module | pins 3/4 |
| 5 | 5 | **A1** | GP27 | GND on module | pins 3/4 |
| 6 | 6 | **A2** | GP28 | GND on module | pins 3/4 |

Breakout PCB: https://www.aliexpress.com/item/1005008956861273.html

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

Future sensors on free header pins (e.g. GP2/GP3 as I2C1):

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
| Grove cable 4-pin | 8+ (2 clock branches + 6 data) |
| PS1240 passive piezo | 1 |
| Raspberry Pi Debug Probe | 1 |
