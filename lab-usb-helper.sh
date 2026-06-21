#!/usr/bin/env bash
# lab-usb-helper.sh — root-only USB helpers for het68 flash/debug workflow.
#
# Called via sudo from fixdebugger.sh (see install-lab-sudoers.sh).
#
# Commands:
#   pico-off     — release drivers, deauthorize Pico USB device (cafe:4066)
#   pico-on      — reauthorize Pico so it can enumerate again
#   probe-reset  — usbreset Debug Probe (2e8a:000c), no unbind/bind cycle
#   wait-probe   — wait until Debug Probe responds to lsusb (exit 0/1)
#   grant-serial — ACL: let the lab user read the probe UART + Pico CDC ttyACMs

set -euo pipefail

PICO_VID="cafe"
PROBE_VID="2e8a"
PROBE_PID="000c"

find_usb_sysfs() {
    local vid="$1"
    local d v
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        v=$(cat "$d/idVendor" 2>/dev/null || true)
        [ "$v" = "$vid" ] && echo "$d" && return 0
    done
    return 1
}

usb_dev_path() {
    # lsusb line: Bus 003 Device 023: ID cafe:4066 ...
    lsusb -d "$1:$2" 2>/dev/null | awk '{printf "/dev/bus/usb/%03d/%03d", $2, $4}' | tr -d ':'
}

unbind_if_bound() {
    local name="$1"
    local node="$2"
    local drv="/sys/bus/usb/drivers/${name}"
    [ -e "${drv}/${node}" ] || return 0
    echo "$node" > "${drv}/unbind" 2>/dev/null || true
}

cmd_pico_off() {
    local path bus
    path=$(find_usb_sysfs "$PICO_VID" 2>/dev/null || true)
    if [ -z "$path" ]; then
        echo "pico-off: Pico USB device not present"
        return 0
    fi
    bus=$(basename "$path")
    echo "pico-off: ${bus} ($(cat "${path}/idVendor"):$(cat "${path}/idProduct"))"

    # Drop userspace holders (ALSA/PipeWire/serial on Pico CDC).
    fuser -k "/dev/snd/pcmC"* 2>/dev/null || true
    fuser -k /dev/ttyACM1 2>/dev/null || true

    # Unbind interface drivers (best-effort).
    local sub
    for sub in "${path}" "${path}:"*; do
        [ -e "$sub" ] || continue
        unbind_if_bound snd-usb-audio "$(basename "$sub")"
        unbind_if_bound cdc_acm "$(basename "$sub")"
    done
    unbind_if_bound usb "$bus"

    # USB reset clears stuck isochronous state on the device side.
    local dev
    dev=$(usb_dev_path "$PICO_VID" "4066" || true)
    if [ -n "$dev" ] && [ -e "$dev" ]; then
        echo "pico-off: usbreset ${dev}"
        usbreset "$dev" 2>/dev/null || true
        sleep 1
    fi

    # Deauthorize — host stops talking to Pico USB; bus can recover for the probe.
    echo 0 > "${path}/authorized"
    echo "pico-off: authorized=0"
    sleep 2
}

cmd_pico_on() {
    local path bus
    path=$(find_usb_sysfs "$PICO_VID" 2>/dev/null || true)
    if [ -z "$path" ]; then
        echo "pico-on: Pico not in sysfs (will enumerate after target reset)"
        return 0
    fi
    bus=$(basename "$path")
    echo 1 > "${path}/authorized"
    echo "pico-on: ${bus} authorized=1"
    sleep 2
}

cmd_probe_reset() {
    local dev
    dev=$(usb_dev_path "$PROBE_VID" "$PROBE_PID" || true)
    if [ -z "$dev" ] || [ ! -e "$dev" ]; then
        echo "probe-reset: Debug Probe not found" >&2
        return 1
    fi
    echo "probe-reset: usbreset ${dev}"
    usbreset "$dev"
    sleep 2
}

cmd_wait_probe() {
    local i path
    for i in $(seq 1 40); do
        if lsusb -d "${PROBE_VID}:${PROBE_PID}" >/dev/null 2>&1; then
            path=$(find_usb_sysfs "$PROBE_VID" 2>/dev/null || true)
            if [ -n "$path" ] && [ "$(cat "${path}/authorized" 2>/dev/null)" = "1" ]; then
                echo "wait-probe: OK (${i})"
                return 0
            fi
        fi
        sleep 0.25
    done
    echo "wait-probe: timeout" >&2
    return 1
}

cmd_grant_serial() {
    # Grant the invoking lab user read/write on the debug serial ports so they
    # can read firmware debug without sudo. ttyACM0 = probe UART, ttyACM1 = Pico
    # CDC. ACLs are transient (cleared on replug) which is fine for a lab.
    local user="${SUDO_USER:-}"
    if [ -z "$user" ]; then
        echo "grant-serial: SUDO_USER not set; run via sudo" >&2
        return 1
    fi
    local t granted=0
    for t in /dev/ttyACM*; do
        [ -e "$t" ] || continue
        if setfacl -m "u:${user}:rw" "$t" 2>/dev/null; then
            echo "grant-serial: ${user} rw ${t}"
            granted=1
        fi
    done
    [ "$granted" = "1" ] || echo "grant-serial: no ttyACM* devices found"
}

usage() {
    echo "Usage: $0 {pico-off|pico-on|probe-reset|wait-probe|grant-serial}" >&2
    exit 2
}

case "${1:-}" in
    pico-off)     cmd_pico_off ;;
    pico-on)      cmd_pico_on ;;
    probe-reset)  cmd_probe_reset ;;
    wait-probe)   cmd_wait_probe ;;
    grant-serial) cmd_grant_serial ;;
    *)            usage ;;
esac
