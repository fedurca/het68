#!/usr/bin/env bash
# build.sh — přepíše err_build.log a loguje úplně vše (configure + verbose build)
#
# Supported board (only):
#   pimoroni_pico_plus2_w_rp2350 — Pimoroni Pico Plus 2 W (RP2350B, 8 MB PSRAM, Wi-Fi/BT)
#
# Example:
#   ./build.sh

set -Eeuo pipefail
set -x

# log do souboru i na konzoli (při každém běhu se přepíše)
exec > >(tee err_build.log) 2>&1

PICO_BOARD="${PICO_BOARD:-pimoroni_pico_plus2_w_rp2350}"
if [ "${PICO_BOARD}" != "pimoroni_pico_plus2_w_rp2350" ]; then
  echo "PICO_BOARD='${PICO_BOARD}' is not supported." >&2
  echo "het68 builds only for pimoroni_pico_plus2_w_rp2350 (Pico Plus 2 W)." >&2
  exit 1
fi
echo "het68: building for PICO_BOARD=${PICO_BOARD}"

# Apply TinyUSB patches (idempotent) before clean build
python3 patches/apply_all.py

# čistý build dir
rm -rf build

# configure + zapni verbose makefiles (pomáhá hlavně u Make)
CMAKE_ARGS=(
  -S . -B build
  -DPICO_BOARD="${PICO_BOARD}"
  -DHET68_DEBUG_CDC="${HET68_DEBUG_CDC:-ON}"
  -DHET68_USB_DIAG="${HET68_USB_DIAG:-OFF}"
  -DHET68_CORE1_SETTLE_SCAN="${HET68_CORE1_SETTLE_SCAN:-OFF}"
  -DCMAKE_VERBOSE_MAKEFILE=ON
)
if [ -n "${HET68_CORE1_SETTLE_MS:-}" ]; then
  CMAKE_ARGS+=(-DHET68_CORE1_SETTLE_MS="${HET68_CORE1_SETTLE_MS}")
fi
if [ -n "${HET68_DOA_EDGE_MM:-}" ]; then
  CMAKE_ARGS+=(-DHET68_DOA_EDGE_MM="${HET68_DOA_EDGE_MM}")
fi
if [ -n "${HET68_DOA_HEIGHT_MM:-}" ]; then
  CMAKE_ARGS+=(-DHET68_DOA_HEIGHT_MM="${HET68_DOA_HEIGHT_MM}")
fi
cmake "${CMAKE_ARGS[@]}"

# build ve verbose režimu (funguje pro Ninja i Make)
cmake --build build --parallel --verbose

