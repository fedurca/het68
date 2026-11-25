#!/usr/bin/env python3
"""Build a device-bound H68P detection plugin blob.

Usage:
  scripts/sign_plugin.py \\
      --payload build/het68_detect.bin \\
      --serial 0011223344556677 \\
      --key signing/dev_secp256k1.pem \\
      --out build/het68_detect-<serial>.h68p \\
      [--entry-off 0] [--bss-size 0] [--rollback 1]

Requires: pip install ecdsa  (secp256k1)
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

try:
    from ecdsa import SigningKey, SECP256k1, util
except ImportError as e:
    sys.stderr.write("sign_plugin.py needs the 'ecdsa' package: pip install ecdsa\n")
    raise SystemExit(1) from e

H68P_MAGIC = 0x50383648  # 'H68P' LE
H68P_VERSION = 1
H68P_API_ABI = 1
HEADER_FMT = "<IIIIIIII8s32sI"  # 8×u32 + serial8 + sha32 + crc = 76
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 76


def crc32_ieee(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return (~crc) & 0xFFFFFFFF


def parse_serial(s: str) -> bytes:
    s = s.strip().replace(":", "").replace("-", "").lower()
    if len(s) != 16 or any(c not in "0123456789abcdef" for c in s):
        raise SystemExit(f"serial must be 16 hex digits, got {s!r}")
    return bytes.fromhex(s)


def build_header(
    *,
    code_size: int,
    entry_off: int,
    bss_size: int,
    rollback: int,
    serial: bytes,
    payload_sha: bytes,
    flags: int = 0,
) -> bytes:
    # hdr_crc32 covers everything before that field.
    pre = struct.pack(
        "<IIIIIIII8s32s",
        H68P_MAGIC,
        H68P_VERSION,
        H68P_API_ABI,
        flags,
        code_size,
        entry_off,
        bss_size,
        rollback,
        serial,
        payload_sha,
    )
    assert len(pre) == 72  # header without hdr_crc32
    crc = crc32_ieee(pre)
    out = pre + struct.pack("<I", crc)
    assert len(out) == HEADER_SIZE
    return out


def sign_blob(header: bytes, payload: bytes, sk: SigningKey) -> bytes:
    msg = header + payload
    digest = hashlib.sha256(msg).digest()
    sig = sk.sign_digest(digest, sigencode=util.sigencode_string)
    if len(sig) != 64:
        raise SystemExit(f"unexpected signature length {len(sig)}")
    return sig


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--payload", required=True, type=Path, help="raw plugin .bin")
    ap.add_argument("--serial", required=True, help="16 hex OTP digits")
    ap.add_argument("--key", required=True, type=Path, help="secp256k1 PEM private key")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--entry-off", type=lambda x: int(x, 0), default=0,
                    help="byte offset of Thumb entry in payload (even; loader ORs Thumb bit)")
    ap.add_argument("--bss-size", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--rollback", type=lambda x: int(x, 0), default=1)
    ap.add_argument("--pub-out", type=Path, default=None,
                    help="optional path to write 64-byte uncompressed pubkey X||Y")
    args = ap.parse_args()

    payload = args.payload.read_bytes()
    if not payload:
        raise SystemExit("empty payload")
    if len(payload) > (512 * 1024 - HEADER_SIZE - 64):
        raise SystemExit("payload too large for 512 KiB H68P slot")
    # Firmware stores a byte offset (even); Core1 entry sets the Thumb bit itself.
    if args.entry_off >= len(payload) or (args.entry_off & 1) != 0:
        raise SystemExit("entry-off must be an even byte offset within payload")

    serial = parse_serial(args.serial)
    sk = SigningKey.from_pem(args.key.read_text())
    if sk.curve != SECP256k1:
        raise SystemExit("key must be secp256k1")
    vk = sk.get_verifying_key()
    pub = vk.to_string()  # 64 bytes X||Y
    if args.pub_out:
        args.pub_out.write_bytes(pub)
        print(f"wrote pubkey {args.pub_out} ({len(pub)} bytes)")

    payload_sha = hashlib.sha256(payload).digest()
    header = build_header(
        code_size=len(payload),
        entry_off=args.entry_off,
        bss_size=args.bss_size,
        rollback=args.rollback,
        serial=serial,
        payload_sha=payload_sha,
    )
    sig = sign_blob(header, payload, sk)
    out = header + payload + sig
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(out)
    print(
        f"wrote {args.out} ({len(out)} bytes) "
        f"serial={serial.hex()} code={len(payload)} entry=0x{args.entry_off:x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
