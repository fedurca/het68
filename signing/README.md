# Firmware integrity and signing

Release builds publish `SHA256SUMS.txt` next to every UF2/ELF so you can verify
downloads:

```bash
sha256sum -c SHA256SUMS.txt
```

## H68P detection plugins (device-bound)

Detection plugins use the **H68P** container (`h68p.h`): OTP serial in the
header, SHA-256 of the payload, ECDSA secp256k1 signature (same curve as RP2350
secure boot).

```bash
./scripts/gen_dev_keys.sh
./build.sh
./scripts/sign_plugin.py \
  --payload build/het68_detect.bin \
  --serial <16 hex from UART serial=> \
  --key signing/dev_secp256k1.pem \
  --out build/device.h68p
```

Upload via CLI `DET PLUGIN WRITE` (see `docs/secure-boot-lab.md`).

Private keys (`signing/*.pem`) are gitignored. The firmware embeds the lab
public key in `h68p_pubkey.c`.

## Platform secure boot

Platform images are sealed/signed with the same secp256k1 key family:

```bash
./scripts/gen_dev_keys.sh
./build.sh
./scripts/seal_platform.sh build/pico_6mic_soundcard.elf
```

Then follow the OTP checklist in `docs/secure-boot-lab.md` /
`scripts/otp_lab_enable.sh`. **OTP writes are irreversible**; keep debug
enabled for lab (do not burn debug-disable).

## Host checksum signatures (releases)

| Asset | Purpose |
|-------|---------|
| `SHA256SUMS.txt` | SHA-256 digests of all firmware binaries |
| `SHA256SUMS.txt.sig` / `.minisig` | Detached signature over the checksum file (planned) |

Until release detached signatures are published, treat `SHA256SUMS.txt` as
integrity-only for GitHub Release assets.
