# Secure boot + H68P lab notes (het68 1.26+)

## Split firmware

| Part | What | Binding |
|------|------|---------|
| **Platform** UF2/ELF | USB UAC2, I2S, CLI, Secure loader, SAU plan | Signed with lab/production secp256k1 (IMAGE_DEF / `picotool seal`) |
| **H68P plugin** | Detection payload (stub today; full `doa` external later) | Header embeds OTP serial; ECDSA secp256k1 over header+payload |

Flash (Plus 2 W, 16 MiB): platform from `0x10000000`, then dual 512 KiB H68P slots, then existing ACID tail. See `het68_flash_map.h`.

TrustZone plan: Core0 / TinyUSB / PSRAM ring = Non-secure; upper 256 KiB SRAM (`0x20040000`) + plugin = Secure. SAU / ACCESSCTRL / NSC veneers live under `secure/`; register writes stay behind `HET68_SAU_PROGRAM=1` until a dual-ELF NS image exists. Lab single-image builds keep the layout as a contract (debug open).

### SRAM footprint (platform + builtin detection, Release)

Measured after `./build.sh` with `arm-none-eabi-size build/pico_6mic_soundcard.elf`:

| | bytes |
|--|--:|
| `.text` | ~435 KiB (flash XIP; `arm-none-eabi-size` ~445668) |
| `.bss` (+data) | ~286 KiB SRAM statics (~292840; detector tables + TinyUSB) |
| PSRAM ring | 6 MiB at `HET68_PSRAM_BASE` (NS) |
| H68P stage | 512 KiB in PSRAM after the ring |
| Plugin Secure window | 256 KiB at `0x20040000` (external load path) |

RP2350 has 520 KiB SRAM — keep detector BSS in the Secure window when switching to `HET68_PLUGIN_EXTERNAL`.

## Lab keys

```bash
./scripts/gen_dev_keys.sh          # signing/dev_secp256k1.pem (gitignored)
# public bytes → signing/dev_secp256k1_pub.bin + h68p_pubkey.c
```

Never commit `*.pem`. Rotating the lab key requires regenerating `h68p_pubkey.c` and resigning plugins.

## Per-device H68P

```bash
./build.sh                         # also builds build/het68_detect.bin
# Read serial= from UART banner / STATUS (16 hex digits)
./scripts/sign_plugin.py \
  --payload build/het68_detect.bin \
  --serial <16hex> \
  --key signing/dev_secp256k1.pem \
  --out build/het68_detect-<serial>.h68p \
  --entry-off 0
```

`entry_off` is an even byte offset into the payload; the Secure loader sets the Thumb bit when forming the Core1 function pointer.

Upload on device (USB audio idle / alt=0):

```
DET PLUGIN WRITE
H68PHEX <hex…>
…
DET PLUGIN END
DET PLUGIN STATUS
```

Wrong serial or bad signature → `DET PLUGIN IMPORT ERR: …`. Detection still starts from **builtin** unless `-DHET68_PLUGIN_REQUIRED=1`.

## Enable secure boot (irreversible)

1. Flash a **signed** platform image that boots with Debug Probe still attached.
2. Confirm UART/SWD work with the signed image **before** burning OTP.
3. Program public key fingerprint into OTP (`BOOTKEY*`) with picotool — see RP2350 datasheet §10.5 and RPi “Understanding RP2350’s security features”.
4. Set `KEY_VALID`, then `CRIT1.SECURE_BOOT_ENABLE`.
5. **Do not** burn debug-disable / disable USB boot yet — lab keeps SWD+UART.

Helper sketch (review before running on a real chip):

```bash
# scripts/otp_lab_enable.sh — intentionally interactive; OTP writes are permanent.
picotool otp load --help
```

## CMake flags

| Flag | Default | Meaning |
|------|---------|---------|
| `HET68_TRUSTZONE` | ON | Call SAU configure stub / CMSE path |
| `HET68_PLUGIN_EXTERNAL` | OFF | Load detection from H68P into Secure SRAM (no builtin `doa`) |
| `HET68_PLUGIN_REQUIRED` | OFF | Do not start Core1 without a valid device H68P |

## Debug

Debug Probe + UART remain the default smoke path (`AGENTS.md`). Production hardening (disable debug) is a later OTP step, not part of 1.26.
