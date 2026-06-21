#!/usr/bin/env bash
# fixdebugger.sh — Prepare USB bus, then flash via Debug Probe (OpenOCD/SWD).
#
# Problem: Pico running UAC2 on the same USB host port tree as the Debug Probe
# can wedge the controller; OpenOCD then reports:
#   "unable to find a matching CMSIS-DAP device"
#
# Linux "unbind" alone is not enough — the Pico PHY may still be active.
# This script deauthorizes + usbresets the Pico USB device, waits for the probe,
# then runs upload.sh.
#
# One-time setup: ./install-lab-sudoers.sh
#
# Usage:
#   ./fixdebugger.sh          — reset USB + flash
#   ./fixdebugger.sh --test   — also run arecord smoke test

set -euo pipefail

ROOT="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HELPER="${ROOT}/lab-usb-helper.sh"
PROBE_VID="2e8a"

kill_holders() {
    pkill -x openocd 2>/dev/null || true
    fuser -k /dev/ttyACM0 2>/dev/null || true
    fuser -k /dev/ttyACM1 2>/dev/null || true
    systemctl --user stop pipewire.socket pipewire wireplumber pipewire-pulse 2>/dev/null || true
}

run_helper() {
    if [ ! -x "$HELPER" ]; then
        echo "ERROR: missing ${HELPER}" >&2
        exit 1
    fi
    sudo "$HELPER" "$@"
}

echo "=== fixdebugger.sh ==="

kill_holders

if ! lsusb -d "${PROBE_VID}:" >/dev/null 2>&1; then
    echo "ERROR: Debug Probe (${PROBE_VID}:*) not found on USB" >&2
    exit 1
fi

echo "--- Pico USB off (release bus for probe) ---"
run_helper pico-off

echo "--- Debug Probe reset ---"
run_helper probe-reset || true

echo "--- Waiting for Debug Probe ---"
if ! run_helper wait-probe; then
    echo "ERROR: Debug Probe not responding." >&2
    echo "Try: unplug/replug the Debug Probe USB cable, then rerun ./fixdebugger.sh" >&2
    run_helper pico-on || true
    exit 1
fi

echo "--- Flashing ---"
if ! "${ROOT}/upload.sh"; then
    echo "ERROR: flash failed" >&2
    run_helper pico-on || true
    exit 1
fi

echo "--- Pico USB on ---"
run_helper pico-on || true

echo "Waiting for Pico to re-enumerate..."
sleep 3
if lsusb -d cafe:4066 >/dev/null 2>&1; then
    echo "Done. Pico USB audio device is back."
else
    echo "Done. Pico firmware flashed; USB device not visible yet (may appear after reset)."
fi

if [ "${1:-}" = "--test" ]; then
    echo
    echo "=== Testing arecord ==="
    sleep 3
    CARD=$(arecord -l 2>/dev/null | awk '/Pico 6ch/{gsub(":","",$2); print $2; exit}')
    CARD=${CARD:-3}
    arecord -D "hw:${CARD},0" -c 6 -r 48000 -f S24_3LE --period-size=480 -d 3 /tmp/test_6ch.wav \
        && echo "=== CAPTURE OK ===" \
        || echo "=== CAPTURE FAILED ==="
fi
