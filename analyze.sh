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
    echo "--- arecord 6ch capture (test hw:3,0) ---"
    arecord -D hw:3,0 -f S16_LE -r 48000 -c 6 -d 10 -v /tmp/pico_6ch.wav
    echo "--- /proc/asound/card3/stream0 ---"
    cat /proc/asound/card3/stream0
    echo ""
} 2>&1 | tee -a connect.log