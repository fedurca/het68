#!/usr/bin/env bash
# build.sh — přepíše err_build.log a loguje úplně vše (configure + verbose build)
#
# Supported board (default and only):
#   pimoroni_pico_plus2_w_rp2350      — Pimoroni Pico Plus 2 W (8 MB PSRAM)
#
# Example:
#   ./build.sh

set -Eeuo pipefail
set -x

# log do souboru i na konzoli (při každém běhu se přepíše)
exec > >(tee err_build.log) 2>&1

PICO_BOARD="${PICO_BOARD:-pimoroni_pico_plus2_w_rp2350}"
case "${PICO_BOARD}" in
  pico|pico_w|pico2|pico2_w)
    echo "PICO_BOARD='${PICO_BOARD}' is not supported." >&2
    echo "het68 requires the Pimoroni Pico Plus 2 W (8 MB PSRAM)." >&2
    echo "Use: PICO_BOARD=pimoroni_pico_plus2_w_rp2350 ./build.sh" >&2
    exit 1
    ;;
  pimoroni_pico_plus2_w_rp2350) ;;
  *)
    echo "Unsupported PICO_BOARD='${PICO_BOARD}'." >&2
    echo "Supported: pimoroni_pico_plus2_w_rp2350" >&2
    exit 1
    ;;
esac
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
