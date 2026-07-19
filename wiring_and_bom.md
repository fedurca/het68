# het68 wiring on RP2350 (Pico 2) — Grove Shield

6-channel USB sound card, 48 kHz / 24-bit (`S24_3LE`), six ICS-43434 microphones
on a **Seeed Grove Shield for Pi Pico v1.0** (SKU 103100142).

**Shield setup:** power switch → **3.3 V** (never 5 V — destroys the mics).

## Grove 4-pin cable — wire colours

Same on **every** Grove cable in this project:

| Pin | Wire colour | Name |
|-----|-------------|------|
| **1** | **white** | signal (lower-number pin) |
| **2** | **yellow** | signal (higher-number pin) |
| **3** | **red** | VCC (3.3 V from shield) |
| **4** | **black** | GND |

## Grove connector map

| Connector | Use | GPIO | Notes |
|---|---|---|---|
| **UART0** | Debug UART → Debug Probe | GP0 TX, GP1 RX | |
| **UART1** | I2S clock bus — mics 1–3 | GP9 SCK, GP8 WS | pin 3 (red) unused |
| **I2C0** | I2S clock bus — mics 4–6 | GP9 SCK, GP8 WS | same pins as UART1; not I2C |
| **I2C1** | PS1240 piezo (H-bridge) | GP6, GP7 | pins 3/4 unused |
| **D16** | Mic 1 data | GP16 SD (pin 1 white) | SEL → GND on module |
| **D18** | Mic 2 data | GP18 SD (pin 1 white) | SEL → GND on module |
| **D20** | Mic 3 data | GP20 SD (pin 1 white) | SEL → GND on module |
| **A0** | Mic 4 data | GP26 SD (pin 1 white) | pin 2 = GP27 (shared with A1 pin 1) |
| **A1** | Mic 5 data | GP27 SD (pin 1 white) | pin 2 = GP28 (shared with A2 pin 1) |
| **A2** | Mic 6 data | GP28 SD (pin 1 white) | pin 2 unused |

---

## Debug UART — connector **UART0** (GP0/GP1)

Grove cable into **UART0**, or direct wires to header pins 1/2/3.

| Pin | Wire | Signal | GPIO | → Debug Probe "U" |
|-----|------|--------|------|-------------------|
| 1 | **white** | UART **RX** | GP1 | ← Probe **TX** (orange) |
| 2 | **yellow** | UART **TX** | GP0 | → Probe **RX** (yellow) |
| 3 | red | NC | — | — |
| 4 | **black** | GND | GND | GND |

```bash
./serial.sh    # 115200 baud
```

---

## I2S clock bus — **UART1** + **I2C0** (GP8/GP9)

Shared **WS** and **SCK** for all six microphones. Connectors **UART1** and **I2C0**
are wired in parallel on the shield (two sockets, one clock bus).

**Do not use red (pin 3)** on clock cables.

| Pin | Wire | Signal | GPIO |
|-----|------|--------|------|
| 1 | **white** | **SCK** | GP9 |
| 2 | **yellow** | **WS** | GP8 |
| 3 | red | NC | — |
| 4 | **black** | GND | GND |

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

Each module: one ICS-43434, **CLK IN** / **CLK OUT** (clock cable), **DATA** (data cable).

```
[CLK IN] ──► ICS-43434 ──► [CLK OUT]     WS, SCK, GND pass through
                │
           [DATA] ─────────────► shield (SD, 3V3, GND)
```

### Clock cable (same colours on every mic)

| Pin | Wire | To ICS-43434 |
|-----|------|--------------|
| 1 white | **SCK** | SCK |
| 2 yellow | **WS** | WS |
| 4 black | **GND** | GND |

### Data cable (same colours on every mic)

| Pin | Wire | To ICS-43434 | Notes |
|-----|------|--------------|-------|
| 1 **white** | **SD** | SD | → shield GPIO (table below) |
| 2 yellow | — | *not used* | SEL hardwired **GND** on PCB |
| 3 **red** | **VDD** | VDD | 3.3 V from shield |
| 4 **black** | **GND** | GND | |

**SEL:** strap **SEL → GND** on every breakout. Firmware captures the left I2S slot only.

### Analog ports A0–A2 — shield chaining

| GPIO | Shield routing |
|---|---|
| **GP26** | A0 pin 1 (white) only |
| **GP27** | A0 pin 2 (yellow) = A1 pin 1 (white) — same net |
| **GP28** | A1 pin 2 (yellow) = A2 pin 1 (white) — same net |

Use **pin 1 (white)** only on each analog port — one mic per socket.

### Data cable — shield socket per mic

| Mic | USB ch | Shield socket | SD GPIO (pin 1 white) |
|---|---|---|---|
| 1 | 1 | **D16** | GP16 |
| 2 | 2 | **D18** | GP18 |
| 3 | 3 | **D20** | GP20 |
| 4 | 4 | **A0** | GP26 |
| 5 | 5 | **A1** | GP27 |
| 6 | 6 | **A2** | GP28 |

Breakout PCB: https://www.aliexpress.com/item/1005008956861273.html

---

## Sync beacon piezo — connector **I2C1** (GP6/GP7)

Passive **PS1240** between pin 1 and pin 2. Do **not** use red or black on this cable.

| Pin | Wire | Signal | GPIO |
|-----|------|--------|------|
| 1 | **white** | Piezo B | GP7 |
| 2 | **yellow** | Piezo A | GP6 |
| 3 | red | NC | — |
| 4 | black | NC | — |

Software H-bridge: one PWM slice, channel B inverted → ±VDD across the element.
No series resistor; keep leads short.

---

## Barometer — Grove DPS310 on **GP2 / GP3** (I2C1)

Optional **Seeed Grove High Precision Barometric Pressure Sensor DPS310**
(product **101020812**, Infineon DPS310). Firmware claims these pins only when
they are free and a device ACKs at I2C `0x77` (default) or `0x76` (pad short).

**Do not** plug this module into Grove Shield **I2C0** (GP8/GP9 = mic clocks) or
**I2C1** (GP6/GP7 = piezo). Wire the Grove cable to the free Pico header pins:

| Grove wire | Colour | Pico pin | Function |
|---|---|---|---|
| 1 | **white** | **GP2** | I2C1 **SDA** |
| 2 | **yellow** | **GP3** | I2C1 **SCL** |
| 3 | **red** | **3V3** | power (shield 3.3 V) |
| 4 | **black** | **GND** | ground |

Range: pressure **300–1200 hPa**, temperature **−40–85 °C**, pressure precision
±0.002 hPa. UART: `BARO` (also in `STATUS` / boot dump / heartbeat `baro=`).
From temperature the firmware computes dry-air **speed of sound**
`c = 331.3√(1+T/273.15)` — shown as `SOUND c=…` / `BARO … c=…m/s` and used
by DOA TDOA when the sensor is present (else 343 m/s).
Wiki: https://wiki.seeedstudio.com/Grove-High-Precision-Barometric-Pressure-Sensor-DPS310/

### Still free / planned on other headers

- INA219 — single-channel current/voltage
- INA3221 — three-channel current/voltage

---

## Bill of materials

| Item | Qty |
|---|---|
| Raspberry Pi Pico 2 (RP2350) | 1 |
| Grove Shield for Pi Pico v1.0 | 1 |
| ICS-43434 breakout / mic module | 6 |
| Grove cable 4-pin | 8+ (2 clock branches + 6 data) + 1 for DPS310 |
| Grove DPS310 barometer (Seeed 101020812) | 1 (optional) |
| PS1240 passive piezo | 1 |
| Raspberry Pi Debug Probe | 1 |
