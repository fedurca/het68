# Test scenarios — het68 1.0.6

Practical checks for wind intensity/direction, UART CLI, time-synced detection
log, and NVR JSON export. Use Debug Probe UART (115200 8N1 on Grove UART0).

## Setup

1. Flash `het68-1.0.6-pico2.uf2` (or local `./build.sh` artifact).
2. Open serial: `picocom -b 115200 /dev/ttyACM1` (or the Debug Probe UART device).
3. On boot (and on first UART byte / USB mount) you should see `=== het68 CLI ===`.
4. Host with USB audio optional for wind/drone tests; flash commits only when
   USB alt setting is 0 (stream idle).

---

## A. CLI help + logging gate

| Step | Action | Expect |
|------|--------|--------|
| A1 | Power on / open UART | Help banner printed |
| A2 | Send `HELP` | Same help text |
| A3 | `LOG OFF` then wait ~2 s with activity | No `SRC` / `ENTITY` / `TRACKS` lines; heartbeat still appears with `log=off` |
| A4 | `LOG ON` | Live `SRC` lines resume |

---

## B. Time sync gate for DET timestamps

| Step | Action | Expect |
|------|--------|--------|
| B1 | Boot, do **not** sync. Create noise (walk/drone). `DET LIST` | `time_synced=0`, empty or only previously flashed events; **no new** timed rows |
| B2 | `TIME` | `synced=0 epoch=0` |
| B3 | `TIME SYNC $(date +%s)` (or paste a unix epoch ≥ 1700000000) | `TIME OK epoch=…` |
| B4 | `TIME` | `synced=1` and increasing epoch |
| B5 | Trigger a classifiable event (walker bout / bird / vehicle / drone) | `DET LIST` shows `first=` / `last=` close to wall clock, `occ≥1`, `max_gap_ms` |

---

## C. Detection CRUD + NVR export

| Step | Action | Expect |
|------|--------|--------|
| C1 | After B3–B5, `DET LIST` | Human-readable `DET id=… class=… first=… last=… occ=… max_gap_ms=…` |
| C2 | `DET EXPORT` | Block `NVREVT BEGIN` … JSON lines … `NVREVT END` |
| C3 | Validate one JSON line | Fields: `v`, `device=het68`, `type=detection`, `id`, `class`, `first_seen`, `last_seen`, `occurrence`, `max_gap_ms`, `az`, `el`, `intensity_db`, `conf` |
| C4 | `DET DEL <id>` | `DET DEL OK`; id gone from `DET LIST` |
| C5 | `DET CLEAR` | All rows cleared |
| C6 | `DET BACKUP` → save hex → `DET IMPORT` / `DETHEX` / `DET END` | Round-trip restore (`DET IMPORT OK`) |
| C7 | Stop USB audio (alt=0), wait | `FLASH: DET ACID commit` when dirty |

### NVR correlation sketch

Pipe export into a smart NVR / SIEM as JSON Lines:

```bash
# Example: capture DET EXPORT output, strip NVREVT markers, feed correlator
grep '^{' uart.log | jq -c 'select(.type=="detection")'
```

Join on `first_seen`/`last_seen` (unix seconds) with camera events ± few seconds;
use `class` + `az`/`el` as side-channel metadata (not pixel coordinates).

---

## D. Wind filter still active + intensity/direction

| Step | Action | Expect |
|------|--------|--------|
| D1 | Quiet indoor, fan **off** | Heartbeat `wind=0`; no `SRC class=wind` |
| D2 | Blow air / point a fan at the array (prefer one face) | `SRC class=wind az=… el=… inten=…dB` (if `LOG ON`); heartbeat `wind=1@…dB` |
| D3 | Rotate fan / blow from opposite side | Reported `az` moves toward that face (± coarse, energy steering) |
| D4 | Strong wind + soft drone-band noise | Drone mics still wind-gated (`DOA_WIND_RATIO`); false drone tracks should not explode |
| D5 | After `TIME SYNC`, repeat D2 | `DET LIST` contains `class=wind` with `inten`, `az`/`el`, rising `occ` |

---

## E. Entity gallery still separate from DET

| Step | Action | Expect |
|------|--------|--------|
| E1 | `ENT LIST` vs `DET LIST` | Gallery = classification templates; DET = timed events |
| E2 | `ENT EXPORT` / `ENT IMPORT` | Unchanged gallery hex protocol |
| E3 | `DET CLEAR` | Does **not** wipe `ENT` gallery |

---

## F. USB audio + opportunistic flash (regression)

| Step | Action | Expect |
|------|--------|--------|
| F1 | Start 6ch USB capture on host | Continuous audio, no multi-second dropouts |
| F2 | While streaming, generate new entities/dets | Heartbeat may show `flash=dirty`; **no** erase/program until alt=0 |
| F3 | Stop capture (alt=0) | `FLASH: ACID commit` and/or `FLASH: DET ACID commit` |
| F4 | Reboot | Gallery + DET log reload from flash |

---

## G. Quick smoke script (host)

```bash
# After serial is open and firmware booted:
printf 'HELP\n' > /dev/ttyACM1
printf 'TIME SYNC %s\n' "$(date +%s)" > /dev/ttyACM1
printf 'LOG ON\n' > /dev/ttyACM1
# … generate acoustic events …
printf 'DET LIST\n' > /dev/ttyACM1
printf 'DET EXPORT\n' > /dev/ttyACM1
```

Pass criteria for a lab smoke: A1–A4, B1–B5, C1–C3, D1–D2, F1–F3.
