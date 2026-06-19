#!/bin/bash

{
    echo "========================================"
    echo "Čas: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "--- dmesg (posledních 10 řádků) ---"
    dmesg -T | tail -n10
    echo "--- arecord -l ---"
    arecord -l
    echo "--- arecord primy test nahravani 3s (480 samples / 10 ms perioda) ---"
    # Prvni pokus: krat zacit nahravanim. --dump-hw-params se vyhybame,
    # protoze dela SET_INTERFACE cycling ktery crashoval firmware.
    arecord -D hw:3,0 -f S16_LE -r 48000 -c 6 \
        --period-size=480 --buffer-size=4800 -d 3 -v /tmp/pico_6ch.wav \
        && echo "=== CAPTURE OK ===" || echo "=== CAPTURE FAILED ==="
    echo "--- dmesg po capture pokusu ---"
    dmesg -T | grep "3-4" | tail -5
    echo "--- /proc/asound/card3/stream0 ---"
    cat /proc/asound/card3/stream0
    echo ""
} 2>&1 | tee -a connect.log