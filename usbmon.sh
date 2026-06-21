#!/usr/bin/env bash
# analyze_usbmon.sh — diagnostika UAC2 Pico 6ch USB Audio + usbmon
# Spouštěj ideálně: sudo ./analyze_usbmon.sh
#
# Co sbírá:
# - dmesg, arecord -l, /proc/asound/cardX/stream0
# - lsusb -v pro VID:PID cafe:4066
# - raw usbmon stopu z konkrétní USB sběrnice během arecord
# - filtrované usbmon řádky jen pro Pico device address
# - jednoduchý souhrn ISO IN / control requestů / chybových statusů

set -u

VIDPID="${VIDPID:-cafe:4066}"
RATE="${RATE:-48000}"
CHANNELS="${CHANNELS:-6}"
FORMAT="${FORMAT:-S24_3LE}"
DURATION="${DURATION:-5}"
OUT_BASE="${OUT_BASE:-/tmp}"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${OUT_BASE}/pico_uac2_usbmon_${TS}"

# Re-exec přes sudo, protože usbmon/debugfs obvykle potřebuje root.
if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    echo "Potřebuji root pro usbmon/debugfs, spouštím přes sudo..."
    exec sudo -E bash "$0" "$@"
fi

mkdir -p "$OUT_DIR"

log() {
    printf '%s\n' "$*" | tee -a "$OUT_DIR/summary.txt"
}

run_cmd() {
    local name="$1"; shift
    {
        echo "===== $name ====="
        echo "\$ $*"
        "$@"
        echo
    } > "$OUT_DIR/${name}.txt" 2>&1
}

log "========================================"
log "Pico UAC2 USBmon diagnostika"
log "Čas: $(date)"
log "VIDPID: $VIDPID"
log "Výstup: $OUT_DIR"
log "========================================"

# Připrav debugfs + usbmon.
if ! mountpoint -q /sys/kernel/debug; then
    log "Mountuji debugfs do /sys/kernel/debug"
    mount -t debugfs none /sys/kernel/debug 2>>"$OUT_DIR/summary.txt" || true
fi

modprobe usbmon 2>>"$OUT_DIR/summary.txt" || true

if [ ! -d /sys/kernel/debug/usb/usbmon ]; then
    log "CHYBA: /sys/kernel/debug/usb/usbmon neexistuje. Kernel nemá dostupný usbmon/debugfs."
    exit 1
fi

# Najdi zařízení podle VID:PID.
LSUSB_LINE="$(lsusb -d "$VIDPID" 2>/dev/null | head -n 1 || true)"
if [ -z "$LSUSB_LINE" ]; then
    log "CHYBA: lsusb nenašel zařízení $VIDPID"
    log "Aktuální lsusb:"
    lsusb | tee -a "$OUT_DIR/summary.txt"
    exit 1
fi

BUS="$(printf '%s\n' "$LSUSB_LINE" | awk '{print $2}')"
DEV_WITH_COLON="$(printf '%s\n' "$LSUSB_LINE" | awk '{print $4}')"
DEV="${DEV_WITH_COLON%:}"
BUS_INT="$((10#$BUS))"
DEV_INT="$((10#$DEV))"
DEV_3="$(printf '%03d' "$DEV_INT")"
USBMON_NODE="/sys/kernel/debug/usb/usbmon/${BUS_INT}u"

log "Nalezeno zařízení:"
log "$LSUSB_LINE"
log "Bus=${BUS_INT}, Device=${DEV_INT}, usbmon=${USBMON_NODE}"

if [ ! -r "$USBMON_NODE" ]; then
    log "CHYBA: Nelze číst $USBMON_NODE"
    log "Dostupné usbmon nody:"
    ls -l /sys/kernel/debug/usb/usbmon | tee -a "$OUT_DIR/summary.txt"
    exit 1
fi

# Najdi ALSA card dynamicky podle názvu Pico.
ARECORD_L="$(arecord -l 2>&1 || true)"
printf '%s\n' "$ARECORD_L" > "$OUT_DIR/arecord_l.txt"

CARD="$(printf '%s\n' "$ARECORD_L" | awk '/Pico 6ch Microphone|P48k16|Pico/ {gsub(":","",$2); print $2; exit}')"
if [ -z "${CARD:-}" ]; then
    log "VAROVÁNÍ: ALSA card nebyla nalezena podle názvu Pico. Budu pokračovat jen s USBmon/lsusb."
    PCM=""
else
    PCM="hw:${CARD},0"
    log "ALSA device: ${PCM}"
fi

# Statické informace před testem.
dmesg | tail -120 > "$OUT_DIR/dmesg_before_tail120.txt" 2>&1 || true
lsusb -v -s "${BUS}:${DEV}" > "$OUT_DIR/lsusb_v_device.txt" 2>&1 || true

if [ -n "${CARD:-}" ] && [ -r "/proc/asound/card${CARD}/stream0" ]; then
    cat "/proc/asound/card${CARD}/stream0" > "$OUT_DIR/stream0_before.txt" 2>&1 || true
fi

# Spusť usbmon capture na celé sběrnici.
RAW="$OUT_DIR/usbmon_bus${BUS_INT}u.raw"
log "Spouštím usbmon capture: $RAW"
stdbuf -oL cat "$USBMON_NODE" > "$RAW" 2>"$OUT_DIR/usbmon_cat.err" &
MON_PID=$!
sleep 0.25

# Dump HW params také vyvolává otevření capture streamu, takže ho sbíráme uvnitř usbmon okna.
if [ -n "${PCM:-}" ]; then
    log "Spouštím arecord --dump-hw-params"
    set +e
    arecord --dump-hw-params -D "$PCM" -f "$FORMAT" -r "$RATE" -c "$CHANNELS" -d 1 \
        "$OUT_DIR/pico_probe.wav" > "$OUT_DIR/arecord_dump_hw_params.log" 2>&1
    DUMP_RC=$?
    set -e
    log "arecord --dump-hw-params exit code: $DUMP_RC"

    sleep 0.25

    log "Spouštím hlavní capture test (${DURATION}s)"
    set +e
    arecord -D "$PCM" -f "$FORMAT" -r "$RATE" -c "$CHANNELS" -d "$DURATION" -v \
        "$OUT_DIR/pico_capture.wav" > "$OUT_DIR/arecord_capture.log" 2>&1 &
    AREC_PID=$!

    # Zkus zachytit stav streamu během běhu. Pokud arecord umře rychle, soubor bude obsahovat už Stop.
    sleep 0.4
    if [ -r "/proc/asound/card${CARD}/stream0" ]; then
        cat "/proc/asound/card${CARD}/stream0" > "$OUT_DIR/stream0_during_capture.txt" 2>&1 || true
    fi

    wait "$AREC_PID"
    CAP_RC=$?
    set -e
    log "arecord capture exit code: $CAP_RC"
else
    DUMP_RC=99
    CAP_RC=99
fi

sleep 0.25
kill "$MON_PID" 2>/dev/null || true
wait "$MON_PID" 2>/dev/null || true
log "USBmon capture ukončen."

dmesg | tail -160 > "$OUT_DIR/dmesg_after_tail160.txt" 2>&1 || true

# Filtrování usbmonu na konkrétní device address.
# Textový usbmon typicky používá tokeny jako Ci:3:020:0 nebo Zi:3:020:1.
FILTERED="$OUT_DIR/usbmon_device_bus${BUS_INT}_dev${DEV_3}.txt"
grep -E ":[0]*${BUS_INT}:${DEV_3}:" "$RAW" > "$FILTERED" 2>/dev/null || true

# Alternativní fallback, kdyby bus nebyl zero/non-zero přesně takto.
if [ ! -s "$FILTERED" ]; then
    grep -E ":${BUS_INT}:${DEV_3}:" "$RAW" > "$FILTERED" 2>/dev/null || true
fi

# Vytáhni užitečné podmnožiny.
grep -E " [SC] [CZ][io]:" "$FILTERED" > "$OUT_DIR/usbmon_control_and_iso.txt" 2>/dev/null || true
grep -E " [SC] Zi:" "$FILTERED" > "$OUT_DIR/usbmon_iso_in.txt" 2>/dev/null || true
grep -E " [SC] Ci:| [SC] Co:" "$FILTERED" > "$OUT_DIR/usbmon_control.txt" 2>/dev/null || true

# Chybové completion/event řádky: status s mínusem. U iso mohou být statusy i ve složeném poli, proto širší grep.
grep -E " [CE] .+ -[0-9]+" "$FILTERED" > "$OUT_DIR/usbmon_errors.txt" 2>/dev/null || true

# Poslední/nejdůležitější řádky pro rychlé vložení do chatu.
{
    echo "===== summary ====="
    echo "Time: $(date)"
    echo "Device: $LSUSB_LINE"
    echo "Bus: $BUS_INT"
    echo "Dev: $DEV_INT / ${DEV_3}"
    echo "ALSA: ${PCM:-not found}"
    echo "arecord dump rc: $DUMP_RC"
    echo "arecord capture rc: $CAP_RC"
    echo
    echo "===== arecord_l ====="
    cat "$OUT_DIR/arecord_l.txt"
    echo
    echo "===== stream0_before ====="
    cat "$OUT_DIR/stream0_before.txt" 2>/dev/null || true
    echo
    echo "===== stream0_during_capture ====="
    cat "$OUT_DIR/stream0_during_capture.txt" 2>/dev/null || true
    echo
    echo "===== arecord_dump_hw_params tail ====="
    tail -80 "$OUT_DIR/arecord_dump_hw_params.log" 2>/dev/null || true
    echo
    echo "===== arecord_capture tail ====="
    tail -120 "$OUT_DIR/arecord_capture.log" 2>/dev/null || true
    echo
    echo "===== dmesg_after_tail160 ====="
    tail -80 "$OUT_DIR/dmesg_after_tail160.txt" 2>/dev/null || true
    echo
    echo "===== usbmon counts ====="
    printf "raw lines: "; wc -l < "$RAW" 2>/dev/null || true
    printf "device lines: "; wc -l < "$FILTERED" 2>/dev/null || true
    printf "iso in lines: "; wc -l < "$OUT_DIR/usbmon_iso_in.txt" 2>/dev/null || true
    printf "control lines: "; wc -l < "$OUT_DIR/usbmon_control.txt" 2>/dev/null || true
    printf "error lines: "; wc -l < "$OUT_DIR/usbmon_errors.txt" 2>/dev/null || true
    echo
    echo "===== usbmon_errors first 80 ====="
    head -80 "$OUT_DIR/usbmon_errors.txt" 2>/dev/null || true
    echo
    echo "===== usbmon_iso_in first 80 ====="
    head -80 "$OUT_DIR/usbmon_iso_in.txt" 2>/dev/null || true
    echo
    echo "===== usbmon_iso_in last 80 ====="
    tail -80 "$OUT_DIR/usbmon_iso_in.txt" 2>/dev/null || true
    echo
    echo "===== usbmon_control last 120 ====="
    tail -120 "$OUT_DIR/usbmon_control.txt" 2>/dev/null || true
} > "$OUT_DIR/report.txt"

# Zabal vše pro snadné poslání.
ARCHIVE="${OUT_DIR}.tar.gz"
tar -czf "$ARCHIVE" -C "$(dirname "$OUT_DIR")" "$(basename "$OUT_DIR")"

log "========================================"
log "Hotovo."
log "Report:  $OUT_DIR/report.txt"
log "Archiv:  $ARCHIVE"
log "========================================"
log ""
log "Sem do chatu pošli ideálně:"
log "  cat $OUT_DIR/report.txt"
log ""
log "Pokud bude moc dlouhý, pošli aspoň:"
log "  grep -n \"usbmon_errors\\|usbmon_iso_in\\|arecord_capture\" -A80 $OUT_DIR/report.txt"
log ""
log "Archiv pro případ potřeby:"
log "  $ARCHIVE"
