#!/usr/bin/env python3
"""Host smoke: sign H68P, accept matching serial, reject wrong serial / bad sig."""
from __future__ import annotations

import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from ecdsa import SECP256k1, VerifyingKey, BadSignatureError
    from ecdsa.util import sigdecode_string
except ImportError as e:
    sys.stderr.write("need ecdsa: pip install ecdsa\n")
    raise SystemExit(1) from e

ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 76
H68P_MAGIC = 0x50383648


def crc32_ieee(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return (~crc) & 0xFFFFFFFF


def verify_blob(blob: bytes, serial: bytes, pub64: bytes) -> str:
    """Return 'ok' or error token matching h68p_status_str()."""
    if len(blob) < HEADER_SIZE + 64:
        return "size"
    magic, ver, abi, _flags, code, entry, _bss, _rb, ser, psha, crc = struct.unpack_from(
        "<IIIIIIII8s32sI", blob, 0
    )
    if magic != H68P_MAGIC:
        return "magic"
    if ver != 1:
        return "version"
    if abi != 1:
        return "abi"
    if code == 0 or HEADER_SIZE + code + 64 > len(blob):
        return "size"
    if entry >= code:
        return "entry"
    if crc32_ieee(blob[:72]) != crc:
        return "crc"
    payload = blob[HEADER_SIZE:HEADER_SIZE + code]
    if hashlib.sha256(payload).digest() != psha:
        return "hash"
    if ser != serial:
        return "serial"
    sig = blob[HEADER_SIZE + code:HEADER_SIZE + code + 64]
    signed = blob[:HEADER_SIZE] + payload
    digest = hashlib.sha256(signed).digest()
    vk = VerifyingKey.from_string(pub64, curve=SECP256k1)
    try:
        if not vk.verify_digest(sig, digest, sigdecode=sigdecode_string):
            return "sig"
    except BadSignatureError:
        return "sig"
    return "ok"


def main() -> int:
    key = ROOT / "signing" / "dev_secp256k1.pem"
    pub_bin = ROOT / "signing" / "dev_secp256k1_pub.bin"
    if not key.is_file() or not pub_bin.is_file():
        subprocess.check_call([str(ROOT / "scripts" / "gen_dev_keys.sh")])
    pub64 = pub_bin.read_bytes()
    assert len(pub64) == 64

    payload = b"\x00\xbf" + b"H68Pstub"  # even entry at 0
    serial_ok = bytes.fromhex("0011223344556677")
    serial_bad = bytes.fromhex("ffffffffffffffff")

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        raw = td / "payload.bin"
        raw.write_bytes(payload)
        out = td / "out.h68p"
        # Odd entry-off must be rejected (matches firmware h68p_parse).
        odd = subprocess.run([
            sys.executable, str(ROOT / "scripts" / "sign_plugin.py"),
            "--payload", str(raw),
            "--serial", serial_ok.hex(),
            "--key", str(key),
            "--out", str(td / "odd.h68p"),
            "--entry-off", "1",
        ], capture_output=True, text=True)
        assert odd.returncode != 0, "odd entry-off should fail"

        subprocess.check_call([
            sys.executable, str(ROOT / "scripts" / "sign_plugin.py"),
            "--payload", str(raw),
            "--serial", serial_ok.hex(),
            "--key", str(key),
            "--out", str(out),
            "--entry-off", "0",
            "--rollback", "3",
        ])
        blob = out.read_bytes()
        magic, ver, abi, flags, code, entry, bss, rb, serial, psha, crc = struct.unpack_from(
            "<IIIIIIII8s32sI", blob, 0
        )
        assert magic == H68P_MAGIC and ver == 1 and abi == 1
        assert code == len(payload) and entry == 0 and (entry & 1) == 0 and rb == 3
        assert serial == serial_ok
        assert psha == hashlib.sha256(payload).digest()
        assert verify_blob(blob, serial_ok, pub64) == "ok"
        assert verify_blob(blob, serial_bad, pub64) == "serial"
        # Flip one signature byte → reject.
        bad = bytearray(blob)
        bad[-1] ^= 0x01
        assert verify_blob(bytes(bad), serial_ok, pub64) == "sig"
        print("H68P round-trip OK; even entry-off OK; wrong-serial/bad-sig reject OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
