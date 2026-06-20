#!/usr/bin/env bash
# Flash RP2350 firmware via Raspberry Pi Debug Probe (CMSIS-DAP + SWD).
#
# Prereq: ./build.sh
# If OpenOCD cannot find the probe, run ./fixdebugger.sh first.

set -euo pipefail

ROOT="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELF="${ROOT}/build/pico_6mic_soundcard.elf"
OPENOCD="${OPENOCD:-/usr/local/bin/openocd}"
SCRIPTS="${OPENOCD_SCRIPTS:-/usr/local/share/openocd/scripts}"
IFACE_CFG="${ROOT}/config/het68-probe.cfg"
RETRIES="${HET68_UPLOAD_RETRIES:-3}"

if [ ! -x "$OPENOCD" ]; then
    echo "ERROR: OpenOCD not found at ${OPENOCD}" >&2
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "ERROR: ${ELF} missing — run ./build.sh first" >&2
    exit 1
fi
if [ ! -f "$IFACE_CFG" ]; then
    echo "ERROR: ${IFACE_CFG} missing" >&2
    exit 1
fi

# Auto-detect Debug Probe serial (avoids picking wrong CMSIS-DAP if several are connected).
if [ -z "${HET68_PROBE_SERIAL:-}" ]; then
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        if [ "$(cat "$d/idVendor" 2>/dev/null)" = "2e8a" ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = "000c" ]; then
            HET68_PROBE_SERIAL="$(cat "$d/serial" 2>/dev/null || true)"
            break
        fi
    done
    export HET68_PROBE_SERIAL
fi

if [ -n "${HET68_PROBE_SERIAL:-}" ]; then
    echo "OpenOCD: probe serial ${HET68_PROBE_SERIAL}"
else
    echo "OpenOCD: probe serial not found (set HET68_PROBE_SERIAL if needed)" >&2
fi

run_openocd() {
    "$OPENOCD" \
        -s "$SCRIPTS" \
        -f "$IFACE_CFG" \
        -f target/rp2350.cfg \
        -c "program ${ELF} verify reset exit"
}

attempt=1
while [ "$attempt" -le "$RETRIES" ]; do
    echo "=== upload attempt ${attempt}/${RETRIES} ==="
    if run_openocd; then
        echo "=== upload OK ==="
        exit 0
    fi
    echo "upload attempt ${attempt} failed" >&2
    attempt=$((attempt + 1))
    [ "$attempt" -le "$RETRIES" ] && sleep 2
done

echo "ERROR: upload failed after ${RETRIES} attempts — try ./fixdebugger.sh" >&2
exit 1
