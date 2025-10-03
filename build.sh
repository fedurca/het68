#!/usr/bin/env bash
# build.sh — přepíše err_build.log a loguje úplně vše (configure + verbose build)

set -Eeuo pipefail
set -x

# log do souboru i na konzoli (při každém běhu se přepíše)
exec > >(tee err_build.log) 2>&1

# čistý build dir
rm -rf build

# configure + zapni verbose makefiles (pomáhá hlavně u Make)
cmake -S . -B build \
  -DPICO_BOARD="${PICO_BOARD:-pico2}" \
  -DCMAKE_VERBOSE_MAKEFILE=ON

# build ve verbose režimu (funguje pro Ninja i Make)
cmake --build build --parallel --verbose

