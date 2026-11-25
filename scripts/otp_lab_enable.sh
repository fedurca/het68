#!/usr/bin/env bash
# otp_lab_enable.sh — interactive checklist before irreversible secure-boot OTP.
# Does NOT burn CRIT flags automatically. See docs/secure-boot-lab.md.
set -Eeuo pipefail

echo "=== het68 secure-boot lab checklist ==="
echo "1. Build & flash a signed platform UF2; confirm UART serial= and SWD still work."
echo "2. Keep Debug Probe connected. Do NOT disable debug OTP rows yet."
echo "3. picotool otp set / seal — program BOOTKEY fingerprint from signing/dev_secp256k1.pem"
echo "4. Mark KEY_VALID only after a known-good boot of the signed image."
echo "5. Only then consider CRIT1.SECURE_BOOT_ENABLE (permanent)."
echo
echo "Ref: RP2350 datasheet §10.5; RPi whitepaper 'Understanding RP2350 security features'."
echo "This script refuses to write OTP. Run picotool manually when ready."
exit 0
