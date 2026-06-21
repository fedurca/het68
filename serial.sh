#!/usr/bin/env bash
# serial.sh — read het68 firmware debug from the Raspberry Pi Debug Probe UART.
#
# The Pico firmware writes debug on UART1 GP8 (TX) @115200. Wire the Debug Probe
# UART connector (konektor „U“):
#   žlutá    Pico GP8 (pin 11) TX  -> Probe RX
#   oranžová Pico GP9 (pin 12) RX <- Probe TX
#   černá    Pico GND (pin 13)    -> Probe GND
# The probe bridges to a host CDC ACM port — usually /dev/ttyACM0.
# This channel is independent of the Pico's own USB, so it keeps working even
# when the UAC2 stack freezes.
#
# Usage:
#   ./serial.sh            # auto-detect probe UART, stream @115200
#   ./serial.sh /dev/ttyACM0
#   BAUD=115200 ./serial.sh
#
# Tip: Pico's own USB CDC (when HET68_DEBUG_CDC=1) is a DIFFERENT port,
#      typically /dev/ttyACM1.

set -uo pipefail

BAUD="${BAUD:-115200}"
PROBE_VID="2e8a"
PROBE_PID="000c"

find_probe_uart() {
    # The Debug Probe (2e8a:000c) exposes a CDC ACM interface used as UART.
    local d iface tty
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        [ "$(cat "$d/idVendor" 2>/dev/null)" = "$PROBE_VID" ] || continue
        [ "$(cat "$d/idProduct" 2>/dev/null)" = "$PROBE_PID" ] || continue
        # Walk its interfaces, find one with a ttyACM* child.
        for iface in "$d":*; do
            [ -d "$iface" ] || continue
            for tty in "$iface"/tty/ttyACM* "$iface"/ttyACM*; do
                [ -e "$tty" ] && { echo "/dev/$(basename "$tty")"; return 0; }
            done
        done
    done
    return 1
}

DEV="${1:-}"
if [ -z "$DEV" ]; then
    DEV="$(find_probe_uart || true)"
fi
if [ -z "$DEV" ]; then
    DEV="/dev/ttyACM0"
    echo "serial.sh: probe UART not auto-detected, falling back to ${DEV}" >&2
fi

if [ ! -e "$DEV" ]; then
    echo "ERROR: ${DEV} does not exist. Is the Debug Probe plugged in?" >&2
    exit 1
fi

echo "=== serial.sh: reading ${DEV} @ ${BAUD} (Ctrl-C to stop) ==="

# Release stale holders (a previous reader, ModemManager, etc.).
fuser -k "$DEV" 2>/dev/null || true
sleep 0.2

# Raw line settings; tolerate failure if we lack permissions (then try sudo).
if ! stty -F "$DEV" "$BAUD" raw -echo 2>/dev/null; then
    echo "serial.sh: stty needs elevated permissions; retrying with sudo" >&2
    sudo stty -F "$DEV" "$BAUD" raw -echo
fi

if [ -r "$DEV" ]; then
    exec cat "$DEV"
else
    echo "serial.sh: ${DEV} not readable by $(id -un); using sudo cat" >&2
    exec sudo cat "$DEV"
fi
