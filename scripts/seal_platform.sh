#!/usr/bin/env bash
# seal_platform.sh — sign/seal the platform ELF/UF2 for lab secure boot.
# Debug remains enabled; this only attaches an IMAGE_DEF signature.
#
# Usage:
#   ./scripts/seal_platform.sh [build/pico_6mic_soundcard.elf]
#
# Requires: picotool on PATH, signing/dev_secp256k1.pem (see gen_dev_keys.sh).
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="${1:-${ROOT}/build/pico_6mic_soundcard.elf}"
KEY="${ROOT}/signing/dev_secp256k1.pem"
OUT_ELF="${ELF%.elf}-signed.elf"
OUT_UF2="${OUT_ELF%.elf}.uf2"

if [[ ! -f "${ELF}" ]]; then
  echo "missing ELF: ${ELF} (run ./build.sh first)" >&2
  exit 1
fi
if [[ ! -f "${KEY}" ]]; then
  echo "missing key: ${KEY} — run ./scripts/gen_dev_keys.sh" >&2
  exit 1
fi
if ! command -v picotool >/dev/null 2>&1; then
  echo "picotool not on PATH" >&2
  exit 1
fi

echo "Sealing ${ELF} → ${OUT_ELF}"
# picotool seal --sign attaches secp256k1 signature for OTP secure boot.
# Keep USB boot / SWD available — do not pass debug-disable flags.
picotool seal --sign "${KEY}" "${ELF}" "${OUT_ELF}"
picotool uf2 convert "${OUT_ELF}" "${OUT_UF2}" --family rp2350-arm-s
echo "wrote ${OUT_ELF}"
echo "wrote ${OUT_UF2}"
echo "Flash the signed UF2, confirm UART/SWD, then see docs/secure-boot-lab.md before OTP."
