#!/usr/bin/env bash
# install-lab-sudoers.sh — passwordless sudo for het68 USB lab scripts.
#
# Installs /etc/sudoers.d/het68-lab so fixdebugger.sh can unbind/bind USB
# devices without a password prompt (uses tee into sysfs).
#
# Usage:
#   ./install-lab-sudoers.sh          # install for current user
#   ./install-lab-sudoers.sh --remove # remove het68-lab sudoers drop-in

set -euo pipefail

SUDOERS_FILE="/etc/sudoers.d/het68-lab"
LAB_USER="${SUDO_USER:-${USER}}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
FIXDEBUGGER="${SCRIPT_DIR}/fixdebugger.sh"

remove() {
    if [ -f "$SUDOERS_FILE" ]; then
        sudo rm -f "$SUDOERS_FILE"
        echo "Removed ${SUDOERS_FILE}"
    else
        echo "Nothing to remove (${SUDOERS_FILE} not present)"
    fi
}

if [ "${1:-}" = "--remove" ]; then
    remove
    exit 0
fi

if [ "$(id -u)" -eq 0 ]; then
    echo "Run as your normal user (not root); the script will call sudo once." >&2
    exit 1
fi

if [ ! -f "$FIXDEBUGGER" ]; then
    echo "ERROR: fixdebugger.sh not found at ${FIXDEBUGGER}" >&2
    exit 1
fi

echo "=== install-lab-sudoers.sh ==="
echo "User: ${LAB_USER}"
echo "Target: ${SUDOERS_FILE}"
echo

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

cat > "$TMP" << EOF
# het68 lab — USB unbind/bind for fixdebugger.sh (generated $(date -Iseconds))
# Narrow rules: tee into usb driver bind/unbind only.
${LAB_USER} ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/usb/drivers/usb/unbind
${LAB_USER} ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/usb/drivers/usb/bind
EOF

sudo cp "$TMP" "$SUDOERS_FILE"
sudo chmod 440 "$SUDOERS_FILE"

if sudo visudo -c -f "$SUDOERS_FILE"; then
    echo
    echo "OK: passwordless sudo installed for USB bind/unbind."
    echo "Test: ${FIXDEBUGGER} (from ${SCRIPT_DIR})"
else
    echo "ERROR: visudo rejected ${SUDOERS_FILE} — removing." >&2
    sudo rm -f "$SUDOERS_FILE"
    exit 1
fi
