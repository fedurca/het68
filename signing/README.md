# Firmware integrity and signing

Release builds publish `SHA256SUMS.txt` next to every UF2/ELF so you can verify
downloads:

```bash
sha256sum -c SHA256SUMS.txt
```

## Future electronic signatures

Signing is not enabled yet. The release workflow already reserves space for
detached signatures without changing the firmware image format.

Planned layout (when a signing key is configured in CI secrets):

| Asset | Purpose |
|-------|---------|
| `SHA256SUMS.txt` | SHA-256 digests of all firmware binaries |
| `SHA256SUMS.txt.sig` / `.minisig` | Detached signature over the checksum file |
| `het68-<ver>-<board>.uf2.sig` | Optional per-image detached signature |

Candidates: [minisign](https://jedisct1.github.io/minisign/), OpenBSD `signify`,
or [cosign](https://github.com/sigstore/cosign) (keyless or key-based).

Until then, treat `SHA256SUMS.txt` as integrity-only (not authenticity).
