#!/usr/bin/env bash
# build.sh — přepíše err_build.log a loguje úplně vše (configure + verbose build)
#
# Supported PICO_BOARD values (default: pico2):
#   pico                              — Raspberry Pi Pico (RP2040)
#   pico_w                            — Raspberry Pi Pico W (RP2040 + CYW43439)
#   pico2                             — Raspberry Pi Pico 2 (RP2350)
#   pico2_w                           — Raspberry Pi Pico 2 W (RP2350 + CYW43439)
#   pimoroni_pico_plus2_w_rp2350      — Pimoroni Pico Plus 2 W
#
# Example:
#   PICO_BOARD=pico ./build.sh
#   PICO_BOARD=pico_w ./build.sh

set -Eeuo pipefail
set -x

# log do souboru i na konzoli (při každém běhu se přepíše)
exec > >(tee err_build.log) 2>&1

PICO_BOARD="${PICO_BOARD:-pico2}"
case "${PICO_BOARD}" in
  pico|pico_w|pico2|pico2_w|pimoroni_pico_plus2_w_rp2350) ;;
  *)
    echo "Unsupported PICO_BOARD='${PICO_BOARD}'." >&2
    echo "Supported: pico pico_w pico2 pico2_w pimoroni_pico_plus2_w_rp2350" >&2
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
  -DHET68_DEBUG_CDC="${HET68_DEBUG_CDC:-OFF}"
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

