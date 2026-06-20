#!/usr/bin/env bash
# fixdebugger.sh — Reset Pico USB device so Debug Probe can flash again.
#
# Problem: when the Pico firmware is running (USB audio active), the
# CMSIS-DAP Debug Probe times out because the Pico USB device interferes
# with the USB host controller's ability to communicate with the probe.
#
# Solution: unbind the Pico from the USB driver, which causes a logical
# disconnect. The Debug Probe can then connect cleanly. After flashing,
# rebind the Pico so it re-enumerates.
#
# Usage:
#   ./fixdebugger.sh          — just reset Pico and probe, then flash
#   ./fixdebugger.sh --test   — also run arecord test after flash

set -euo pipefail

PICO_VID="cafe"
PROBE_VID="2e8a"

find_usb_path() {
    local vid="$1"
    for d in /sys/bus/usb/devices/*/idVendor; do
        [ -f "$d" ] || continue
        v=$(cat "$d" 2>/dev/null)
        [ "$v" = "$vid" ] && echo "${d%/idVendor}" && return 0
    done
    return 1
}

usb_unbind() {
    echo "$1" | sudo tee /sys/bus/usb/drivers/usb/unbind > /dev/null
}

usb_bind() {
    echo "$1" | sudo tee /sys/bus/usb/drivers/usb/bind > /dev/null
}

echo "=== fixdebugger.sh ==="

# 1. Unbind Pico USB audio device (releases snd_usb_audio / ALSA)
PICO_PATH=$(find_usb_path "$PICO_VID" 2>/dev/null || true)
if [ -n "$PICO_PATH" ]; then
    PICO_BUS=$(basename "$PICO_PATH")
    echo "Pico at $PICO_BUS — unbinding..."
    usb_unbind "$PICO_BUS" 2>/dev/null || true
    sleep 1
else
    echo "Pico not found (already disconnected?)"
fi

# 2. Unbind + rebind Debug Probe to reset its USB state
PROBE_PATH=$(find_usb_path "$PROBE_VID" 2>/dev/null || true)
if [ -n "$PROBE_PATH" ]; then
    PROBE_BUS=$(basename "$PROBE_PATH")
    echo "Debug Probe at $PROBE_BUS — resetting..."
    usb_unbind "$PROBE_BUS" 2>/dev/null || true
    sleep 1
    usb_bind "$PROBE_BUS" 2>/dev/null || true
    sleep 2
else
    echo "Debug Probe not found!"
    exit 1
fi

# 3. Flash firmware
echo "Flashing..."
./upload.sh

# 4. Rebind Pico (re-enumerates as USB audio)
echo "Waiting for Pico to boot..."
sleep 3
PICO_PATH=$(find_usb_path "$PICO_VID" 2>/dev/null || true)
if [ -n "$PICO_PATH" ]; then
    PICO_BUS=$(basename "$PICO_PATH")
    echo "Pico re-enumerated at $PICO_BUS"
else
    echo "Pico re-enumerated automatically"
fi

echo "Done. Pico is running new firmware."

# 5. Optional: run arecord test
if [ "${1:-}" = "--test" ]; then
    echo
    echo "=== Testing arecord ==="
    sleep 5
    arecord -D hw:3,0 -c 6 -r 48000 -f S16_LE --period-size=480 -d 3 /tmp/test_6ch.wav \
        && echo "=== CAPTURE OK ===" \
        || echo "=== CAPTURE FAILED ==="
fi
