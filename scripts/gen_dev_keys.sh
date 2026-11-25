#!/usr/bin/env bash
# Generate a lab secp256k1 keypair for H68P + platform signing.
# Private key is gitignored; public key bytes are committed for the firmware.
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${ROOT}/signing"
PRIV="${ROOT}/signing/dev_secp256k1.pem"
PUB_TXT="${ROOT}/signing/dev_secp256k1_pub.hex"
PUB_BIN="${ROOT}/signing/dev_secp256k1_pub.bin"
PUB_C="${ROOT}/h68p_pubkey.c"

if [[ -f "${PRIV}" ]]; then
  echo "keeping existing ${PRIV}"
else
  openssl ecparam -name secp256k1 -genkey -noout -out "${PRIV}"
  chmod 600 "${PRIV}"
  echo "wrote ${PRIV}"
fi

python3 - <<'PY' "${PRIV}" "${PUB_TXT}" "${PUB_BIN}" "${PUB_C}"
import sys
from pathlib import Path
from ecdsa import SigningKey, SECP256k1
priv, pub_txt, pub_bin, pub_c = map(Path, sys.argv[1:])
sk = SigningKey.from_pem(priv.read_text())
assert sk.curve == SECP256k1
pub = sk.get_verifying_key().to_string()  # 64 bytes
pub_bin.write_bytes(pub)
pub_txt.write_text(pub.hex() + "\n")
lines = [
    "// Auto-generated from signing/dev_secp256k1_pub.bin — do not edit by hand.",
    '#include "h68p_pubkey.h"',
    "",
    "const uint8_t HET68_H68P_PUBKEY[H68P_PUBKEY_LEN] = {",
]
for i in range(0, 64, 8):
    chunk = ", ".join(f"0x{b:02x}" for b in pub[i:i+8])
    lines.append(f"    {chunk},")
lines.append("};")
lines.append("")
pub_c.write_text("\n".join(lines))
print(f"wrote {pub_bin}, {pub_txt}, {pub_c}")
PY
