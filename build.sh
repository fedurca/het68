#!/usr/bin/env bash
# build.sh — přepíše err_build.log a loguje úplně vše (configure + verbose build)

set -Eeuo pipefail
set -x

# log do souboru i na konzoli (při každém běhu se přepíše)
exec > >(tee err_build.log) 2>&1

# Apply TinyUSB patches (idempotent) before clean build
python3 patches/apply_all.py

# čistý build dir
rm -rf build

# configure + zapni verbose makefiles (pomáhá hlavně u Make)
CMAKE_ARGS=(
  -S . -B build
  -DPICO_BOARD="${PICO_BOARD:-pico2}"
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
cmake "${CMAKE_ARGS[@]}"

# build ve verbose režimu (funguje pro Ninja i Make)
cmake --build build --parallel --verbose

