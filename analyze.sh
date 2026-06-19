#!/bin/bash

{
    echo "========================================"
    echo "Čas: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "--- dmesg (posledních 10 řádků) ---"
    dmesg -T | tail -n10
    echo "--- arecord -l ---"
    arecord -l
    echo "--- arecord --dump-hw-params (test hw:3,0) ---"
    arecord --dump-hw-params -D hw:3,0 -f S16_LE -r 48000 -c 6 -d 1 /tmp/pico_probe.wav
    echo "--- arecord 6ch capture 2s (USB-aligned period, hw:3,0) ---"
    # USB audio needs periods aligned to 1 ms (48 samples at 48 kHz).
    # arecord default 125 ms period fails set_params; use --period-size=480 (10 ms).
    arecord -D hw:3,0 -f S16_LE -r 48000 -c 6 --period-size=480 -d 2 -v /tmp/pico_6ch.wav
    echo "--- /proc/asound/card3/stream0 ---"
    cat /proc/asound/card3/stream0
    echo ""
} 2>&1 | tee -a connect.log