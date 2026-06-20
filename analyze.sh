#!/bin/bash
# Smoke test: enumerate Pico UAC2 card and capture 3 s of 6ch audio.

cleanup() {
    if [ "${WP_STOPPED:-0}" = 1 ]; then
        systemctl --user start pipewire-pulse wireplumber 2>/dev/null || true
    fi
}
trap cleanup EXIT

{
    echo "========================================"
    echo "Čas: $(date '+%Y-%m-%d %H:%M:%S')"

    CARD=$(arecord -l 2>/dev/null | awk '/Pico 6ch/{gsub(":","",$2); print $2; exit}')
    CARD=${CARD:-3}
    PCM_DEV="/dev/snd/pcmC${CARD}D0c"
    echo "--- ALSA card: ${CARD} (${PCM_DEV}) ---"

    echo "--- dmesg (posledních 10 řádků) ---"
    dmesg -T | tail -n10
    echo "--- arecord -l ---"
    arecord -l

    # WirePlumber grabs USB audio on plug; stop it so arecord can open hw directly.
    WP_STOPPED=0
    if systemctl --user is-active --quiet wireplumber 2>/dev/null \
       || systemctl --user is-active --quiet pipewire 2>/dev/null; then
        echo "--- stopping pipewire/wireplumber for exclusive hw capture ---"
        systemctl --user stop pipewire.socket pipewire wireplumber pipewire-pulse 2>/dev/null && WP_STOPPED=1
        sleep 2
        pkill -9 wireplumber 2>/dev/null || true
        sleep 1
    fi

    echo "--- arecord primy test nahravani 3s (480 samples / 10 ms perioda) ---"
    arecord -D "hw:${CARD},0" -f S16_LE -r 48000 -c 6 \
        --period-size=480 --buffer-size=4800 -d 3 -v /tmp/pico_6ch.wav \
        && echo "=== CAPTURE OK ===" || echo "=== CAPTURE FAILED ==="

    if [ -f /tmp/pico_6ch.wav ]; then
        ls -la /tmp/pico_6ch.wav
    fi

    echo "--- dmesg po capture pokusu ---"
    dmesg -T | grep "3-3" | tail -5
    echo "--- /proc/asound/card${CARD}/stream0 ---"
    cat "/proc/asound/card${CARD}/stream0" 2>/dev/null || echo "(card not present)"
    echo ""
} 2>&1 | tee -a connect.log
