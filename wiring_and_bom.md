# het68 wiring on RP2350 (Pico 2)

6-channel USB sound card, 48 kHz / 24-bit (`S24_3LE`), six ICS-43434 microphones
wired as three stereo pairs.

## ICS-43434 microphone power

**WARNING:** Power from **3V3 (OUT), Pin 36** — not VSYS!

| Signal | Pico | Microphones |
|---|---|---|
| VDD | Pin 36 (3V3 OUT) | all 6× VDD |
| GND | Pin 38 (GND) | all 6× GND |

## I2S — Pico is master

| Pico | Signal | To |
|---|---|---|
| GP0 (Pin 1) | WS / LRCLK | all microphones |
| GP1 (Pin 2) | SCK / BCLK | all microphones |
| GP2 (Pin 4) | SD | mics 1+2 (shared data line) |
| GP3 (Pin 5) | SD | mics 3+4 |
| GP4 (Pin 6) | SD | mics 5+6 |

### SEL (L/R channel select)

| Microphone | SEL |
|---|---|
| 1, 3, 5 | GND (left channel) |
| 2, 4, 6 | 3V3 (right channel) |

Breakout: https://www.aliexpress.com/item/1005008956861273.html

## Debug UART — Raspberry Pi Debug Probe

Independent of I2S (UART1, GP8/GP9). Cable: **yellow**, **orange**, **black**.

| Wire | Pico | Pin | Signal | Debug Probe "U" |
|---|---|---|---|---|
| **yellow** | GP8 | 11 | UART TX | → Probe **RX** |
| **orange** | GP9 | 12 | UART RX | ← Probe **TX** |
| **black** | GND | 13 | ground | GND |

```bash
./serial.sh    # 115200 baud, /dev/ttyACM0
```

## Planned peripherals (I2C)

- BME280 — temperature, pressure, humidity
- INA219 — single-channel current/voltage
- INA3221 — three-channel current/voltage
- HITPOINT PT-2038WQ — piezo (no driver)
