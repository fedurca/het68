#!/usr/bin/env bash
# setup_pi_build_env.sh — het68 cross-build environment on Raspberry Pi OS / Debian (ARM).
#
# Target: headless build host (e.g. Pi Zero 2 W over Tailscale). No Debug Probe required.
# Run on the Pi as a user with sudo:
#   chmod +x setup_pi_build_env.sh
#   ./setup_pi_build_env.sh
#
# Options:
#   --purge-heavy   Remove Chromium, Firefox, Wolfram, RealVNC (default: yes)
#   --purge-gui     Also remove desktop metapackage (lxde / labwc stack) for ~1+ GiB
#   --no-clone      Skip git clone (repo already present)
#   --no-build      Install deps only, do not run ./build.sh
#   --repo-url URL  Git remote (default: https://github.com/fedurca/het68.git)
#   --repo-dir DIR  Clone directory (default: ~/het68)
#
# From lab PC (SSH):
#   scp setup_pi_build_env.sh zp2:
#   ssh -t zp2 './setup_pi_build_env.sh'
#
# SSH config on lab PC (~/.ssh/config):
#   Host zp2
#       HostName 100.75.68.52
#       User fedurca
#       IdentityFile ~/.ssh/id_ed25519
#       ServerAliveInterval 60

set -Eeuo pipefail

PURGE_HEAVY=1
PURGE_GUI=0
DO_CLONE=1
DO_BUILD=1
REPO_URL="${HET68_REPO_URL:-https://github.com/fedurca/het68.git}"
REPO_DIR="${HET68_REPO_DIR:-$HOME/het68}"

while [ $# -gt 0 ]; do
    case "$1" in
        --purge-heavy) PURGE_HEAVY=1 ;;
        --no-purge-heavy) PURGE_HEAVY=0 ;;
        --purge-gui) PURGE_GUI=1 ;;
        --no-clone) DO_CLONE=0 ;;
        --no-build) DO_BUILD=0 ;;
        --repo-url) shift; REPO_URL="${1:?}" ;;
        --repo-dir) shift; REPO_DIR="${1:?}" ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

log() { printf '=== %s ===\n' "$*"; }
need_root() {
    if [ "$(id -u)" -eq 0 ]; then
        SUDO=""
    elif sudo -n true 2>/dev/null; then
        SUDO="sudo"
    else
        echo "sudo password required — run: ssh -t zp2 './setup_pi_build_env.sh'" >&2
        exec sudo "$0" "$@"
    fi
}
need_root "$@"

MIN_FREE_MB=900

free_mb() {
    df -m / | awk 'NR==2 {print $4}'
}

purge_heavy_browsers() {
    log "Removing heavy packages (Chromium, Firefox, VNC, Wolfram, …)"
    local pkgs=()
    for p in chromium chromium-common chromium-l10n chromium-sandbox rpi-chromium-mods \
             firefox rpi-firefox-mods realvnc-vnc-server \
             wolfram-engine wolfram-engine-core raspberrypi-ui-mods; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" && pkgs+=("$p")
    done
    if [ "${#pkgs[@]}" -gt 0 ]; then
        $SUDO DEBIAN_FRONTEND=noninteractive apt-get purge -y "${pkgs[@]}"
    else
        echo "  (none of the heavy packages installed)"
    fi
}

purge_gui() {
    log "Removing desktop GUI stack (headless build host)"
    local pkgs=()
    for p in raspberrypi-ui-mods rpd-x rpd-wayland-core lxde lxde-core \
             labwc rpi-chromium-mods pcmanfm; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" && pkgs+=("$p")
    done
    if [ "${#pkgs[@]}" -gt 0 ]; then
        $SUDO DEBIAN_FRONTEND=noninteractive apt-get purge -y "${pkgs[@]}"
    fi
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get autoremove -y
}

ensure_swap() {
    log "Ensuring swap for low-RAM Pi (Pi Zero 2 / 512 MiB)"
    local want_mb=1024
    local cur_mb
    cur_mb=$(($(wc -c < /proc/swaps 2>/dev/null || echo 0) / 1024 / 1024))
    if [ -f /proc/swaps ] && awk 'NR>1 {s+=$3} END {print s+0}' /proc/swaps | grep -q .; then
        cur_mb=$(awk 'NR>1 {s+=$3} END {print int(s/1024)+0}' /proc/swaps)
    fi
    if [ "${cur_mb:-0}" -lt "$want_mb" ] && [ -z "${HET68_SKIP_SWAP:-}" ]; then
        if [ ! -f /swapfile ] || [ "$(stat -c%s /swapfile 2>/dev/null || echo 0)" -lt $((want_mb * 1024 * 1024)) ]; then
            echo "  Creating ${want_mb} MiB /swapfile (needs root, may take a minute)"
            $SUDO fallocate -l "${want_mb}M" /swapfile 2>/dev/null \
                || $SUDO dd if=/dev/zero of=/swapfile bs=1M count="$want_mb" status=progress
            $SUDO chmod 600 /swapfile
            $SUDO mkswap /swapfile
            grep -q '/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' | $SUDO tee -a /etc/fstab
        fi
        $SUDO swapon /swapfile 2>/dev/null || true
    fi
    free -h
}

install_build_deps() {
    log "Installing build dependencies"
    $SUDO apt-get update -qq
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y \
        build-essential \
        cmake \
        git \
        python3 \
        make \
        ninja-build \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        pkg-config
    $SUDO DEBIAN_FRONTEND=noninteractive dpkg --configure -a || true
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y -f
    $SUDO apt-get clean
}

verify_toolchain() {
    log "Verifying ARM toolchain"
    command -v arm-none-eabi-gcc >/dev/null
    command -v cmake >/dev/null
    command -v python3 >/dev/null
    arm-none-eabi-gcc --version | head -1
    test -n "$(find /usr/lib/arm-none-eabi/newlib -name libc.a 2>/dev/null | head -1)"
    echo "  newlib: OK"
}

clone_repo() {
    log "Cloning het68 -> ${REPO_DIR}"
    if [ -d "${REPO_DIR}/.git" ]; then
        echo "  repo exists, fetching"
        git -C "${REPO_DIR}" fetch --depth 1 origin master 2>/dev/null || true
        git -C "${REPO_DIR}" checkout master 2>/dev/null || true
        git -C "${REPO_DIR}" pull --ff-only 2>/dev/null || true
    else
        git clone --depth 1 --branch master "${REPO_URL}" "${REPO_DIR}"
    fi
}

run_build() {
    log "Building het68 (low parallelism on small Pi)"
    local jobs=1
    local mem_mb
    mem_mb=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)
    if [ "$mem_mb" -ge 3500 ]; then
        jobs=4
    elif [ "$mem_mb" -ge 1500 ]; then
        jobs=2
    fi
    echo "  MemTotal=${mem_mb} MiB -> CMAKE_BUILD_PARALLEL_LEVEL=${jobs}"
    cd "${REPO_DIR}"
    export CMAKE_BUILD_PARALLEL_LEVEL="${jobs}"
    ./build.sh
    test -f build/pico_6mic_soundcard.uf2
    ls -la build/pico_6mic_soundcard.{uf2,elf}
}

# --- main ---
log "het68 Pi build environment setup"
uname -a
df -h /

if [ "$PURGE_HEAVY" -eq 1 ]; then
    purge_heavy_browsers
    $SUDO apt-get autoremove -y
fi
if [ "$PURGE_GUI" -eq 1 ]; then
    purge_gui
fi
$SUDO apt-get clean

avail=$(free_mb)
echo "Free disk: ${avail} MiB (want >= ${MIN_FREE_MB} MiB for clone+build)"
if [ "$avail" -lt "$MIN_FREE_MB" ]; then
    echo "WARNING: low disk — re-run with --purge-gui or use a larger SD card." >&2
    if [ "$PURGE_GUI" -eq 0 ]; then
        echo "  Example: ./setup_pi_build_env.sh --purge-gui" >&2
    fi
fi

ensure_swap
install_build_deps
verify_toolchain

if [ "$DO_CLONE" -eq 1 ]; then
    clone_repo
fi

if [ "$DO_BUILD" -eq 1 ]; then
    run_build
fi

log "Done"
echo "Next: cd ${REPO_DIR} && ./build.sh"
echo "Flash (on a machine with Debug Probe): ./fixdebugger.sh"
