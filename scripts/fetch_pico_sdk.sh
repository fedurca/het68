#!/usr/bin/env bash
# fetch_pico_sdk.sh — clone/checkout the Pico SDK pin used by this project.
#
# Pico SDK is gitignored (local clone / CI fetch), not committed as a submodule.
# Pin matches README / PATCHES.md: Pico SDK 2.2.0 + TinyUSB 0.18.0.
#
# Usage (from repo root or anywhere):
#   ./scripts/fetch_pico_sdk.sh
#   PICO_SDK_REF=2.2.0 ./scripts/fetch_pico_sdk.sh   # override pin (advanced)

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${ROOT}/pico-sdk"
# Pico SDK 2.2.0 release tag (commit a1438dff…, ships TinyUSB 0.18.0).
# Keep in sync with README.md / PATCHES.md / UPSTREAM_DELTA.md.
PICO_SDK_REF="${PICO_SDK_REF:-2.2.0}"
PICO_SDK_URL="${PICO_SDK_URL:-https://github.com/raspberrypi/pico-sdk.git}"

if [ -d "${SDK_DIR}/.git" ]; then
  echo "pico-sdk: checking out ${PICO_SDK_REF}"
  git -C "${SDK_DIR}" fetch --tags --depth 1 origin "refs/tags/${PICO_SDK_REF}:refs/tags/${PICO_SDK_REF}" 2>/dev/null \
    || git -C "${SDK_DIR}" fetch --depth 1 origin "${PICO_SDK_REF}"
  git -C "${SDK_DIR}" checkout --force "${PICO_SDK_REF}"
else
  echo "pico-sdk: cloning ${PICO_SDK_REF}"
  rm -rf "${SDK_DIR}"
  git clone --depth 1 --branch "${PICO_SDK_REF}" "${PICO_SDK_URL}" "${SDK_DIR}"
fi

echo "pico-sdk: init tinyusb submodule"
git -C "${SDK_DIR}" submodule update --init --depth 1 lib/tinyusb

# Needed for CYW43 / BLE OpenDroneID builds (Pico W, Pico Plus 2 W).
echo "pico-sdk: init btstack + cyw43-driver submodules"
git -C "${SDK_DIR}" submodule update --init --depth 1 lib/btstack lib/cyw43-driver

echo "pico-sdk: OK @ $(git -C "${SDK_DIR}" rev-parse HEAD) (${PICO_SDK_REF})"
