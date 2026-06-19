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
    echo "--- arecord 2s minimal period (1 USB frame = 48 samples = 1 ms) ---"
    # period-size=48 = 1 USB frame; buffer-size=96 = 2 periods (minimum).
    # This is the most conservative config — if this fails, it is a firmware issue.
    arecord -D hw:3,0 -f S16_LE -r 48000 -c 6 \
        --period-size=48 --buffer-size=96 -d 2 -v /tmp/pico_6ch_min.wav \
        && echo "=== CAPTURE OK ===" || echo "=== CAPTURE FAILED ==="
    echo "--- arecord 2s standard period (10 ms) ---"
    arecord -D hw:3,0 -f S16_LE -r 48000 -c 6 \
        --period-size=480 --buffer-size=4800 -d 2 -v /tmp/pico_6ch.wav \
        && echo "=== CAPTURE OK ===" || echo "=== CAPTURE FAILED ==="
    echo "--- /proc/asound/card3/stream0 ---"
    cat /proc/asound/card3/stream0
    echo ""
} 2>&1 | tee -a connect.log