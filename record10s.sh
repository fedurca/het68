#!/usr/bin/env bash
# record10s.sh — capture 10 s of 6ch / 48 kHz / 24-bit (S24_3LE) from het68.
#
# Stops PipeWire/WirePlumber so arecord can open the Pico UAC2 device directly.
# Restarts the user audio stack on exit when this script stopped it.
#
# Usage:
#   ./record10s.sh                    # -> /tmp/lab_6ch_10s.wav
#   ./record10s.sh /path/to/out.wav
#   OUT_FILE=/tmp/foo.wav ./record10s.sh

set -euo pipefail

ROOT="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_FILE="${1:-${OUT_FILE:-/tmp/lab_6ch_10s.wav}}"
DURATION="${DURATION:-10}"
RATE="${RATE:-48000}"
CHANNELS="${CHANNELS:-6}"
FORMAT="${FORMAT:-S24_3LE}"
WP_STOPPED=0

cleanup() {
    if [ "${WP_STOPPED}" = 1 ]; then
        systemctl --user start pipewire-pulse wireplumber 2>/dev/null || true
    fi
}
trap cleanup EXIT

find_card() {
    arecord -l 2>/dev/null | awk '/Pico 6ch/{gsub(":","",$2); print $2; exit}'
}

stop_audio_stack() {
    if systemctl --user is-active --quiet wireplumber 2>/dev/null \
       || systemctl --user is-active --quiet pipewire 2>/dev/null; then
        echo "Stopping pipewire/wireplumber for exclusive hw capture..."
        systemctl --user stop pipewire.socket pipewire wireplumber pipewire-pulse 2>/dev/null && WP_STOPPED=1
        sleep 1
        pkill -9 wireplumber 2>/dev/null || true
        sleep 0.5
    fi
    fuser -k /dev/snd/* 2>/dev/null || true
    sleep 0.5
}

CARD="$(find_card || true)"
CARD="${CARD:-3}"

echo "=== record10s.sh ==="
echo "Card:    ${CARD} (Pico 6ch Microphone 48k/24)"
echo "Output:  ${OUT_FILE}"
echo "Format:  ${CHANNELS}ch @ ${RATE} Hz, ${FORMAT}, ${DURATION} s"

if ! arecord -l 2>/dev/null | grep -q 'Pico 6ch'; then
    echo "WARNING: Pico 6ch card not found in arecord -l; trying card ${CARD} anyway" >&2
fi

stop_audio_stack

arecord -D "hw:${CARD},0" \
    -f "${FORMAT}" \
    -r "${RATE}" \
    -c "${CHANNELS}" \
    --period-size=480 \
    --buffer-size=4800 \
    -d "${DURATION}" \
    -v \
    "${OUT_FILE}"

ls -la "${OUT_FILE}"
echo "=== CAPTURE OK: ${OUT_FILE} ==="
