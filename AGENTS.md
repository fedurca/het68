# het68 AI agent rules

## Project

This is a Raspberry Pi Pico 2 USB audio firmware project (RP2350B only).

Goal:
- 6-channel USB audio input
- 3x stereo I2S microphone pairs
- I2S RX via PIO
- DMA transfer into memory
- TinyUSB USB Audio device
- 6 MiB DOA ring in the 8 MB PSRAM

## Hard rules

- Do not edit build/, generated/, pico-sdk/ or picotool/ unless explicitly asked.
- Do not remove checks or tests to make the build pass.
- Do not introduce malloc/free into the realtime audio path.
- Do not block in IRQ handlers, DMA callbacks or USB audio callbacks.
- Do not change pin mappings without updating documentation.
- Keep USB descriptors and tusb_config.h consistent.
- Keep PIO, DMA and USB frame sizes consistent.
- Prefer small, reviewable changes.
- After firmware code changes, run ./build.sh.

## Build

Build command:

./build.sh

Default and only target board:

PICO_BOARD=pimoroni_pico_plus2_w_rp2350

Not supported: pico, pico_w (RP2040), pico2, pico2_w (no 8 MB PSRAM).

## Flash/debug assumption

Raspberry Pi Debug Probe is connected externally.

Preferred future flow:
- build ELF/UF2
- flash ELF via OpenOCD/SWD
- use UART/serial logging for smoke tests

## Files/directories AI should usually ignore

- build/
- generated/
- pico-sdk/
- picotool/
- *.uf2
- *.elf
- *.bin
- *.map
- *.log

## Definition of done

- CMake configure passes.
- Firmware build passes.
- ELF/UF2 is produced.
- No generated/vendor files are modified unintentionally.
- Realtime audio constraints remain respected.
