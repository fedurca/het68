# Zapojení het68 na RP2350 (Pico 2)

## Napájení mikrofonů ICS-43434

**POZOR:** Napájení na **3V3 (OUT) Pin 36**, ne VSYS!

| Signál | Pico | Mikrofony |
|---|---|---|
| VDD | Pin 36 (3V3 OUT) | všech 6× VDD |
| GND | Pin 38 (GND) | všech 6× GND |

## I2S — Pico je master

| Pico | Signál | Kam |
|---|---|---|
| GP0 (Pin 1) | WS / LRCLK | všechny mikrofony |
| GP1 (Pin 2) | SCK / BCLK | všechny mikrofony |
| GP2 (Pin 4) | SD | mikrofony 1+2 (sdílená datová linka) |
| GP3 (Pin 5) | SD | mikrofony 3+4 |
| GP4 (Pin 6) | SD | mikrofony 5+6 |

### SEL (výběr kanálu L/R)

| Mikrofon | SEL |
|---|---|
| 1, 3, 5 | GND (levý kanál) |
| 2, 4, 6 | 3V3 (pravý kanál) |

Breakout: https://www.aliexpress.com/item/1005008956861273.html

## Debug UART — Raspberry Pi Debug Probe

Nezávislé na I2S (UART1, GP8/GP9). Kabel: **žlutá**, **oranžová**, **černá**.

| Barva | Pico | Pin | Signál | Debug Probe „U“ |
|---|---|---|---|---|
| **žlutá** | GP8 | 11 | UART TX | → Probe **RX** |
| **oranžová** | GP9 | 12 | UART RX | ← Probe **TX** |
| **černá** | GND | 13 | zem | GND |

```bash
./serial.sh    # 115200 baud, /dev/ttyACM0
```

## Plánované periferie (I2C)

- BME280 — teplota, tlak, vlhkost
- INA219 — jednokanálový proud/napětí
- INA3221 — tříkanálový proud/napětí
- HITPOINT PT-2038WQ — piezo (bez budiče)
