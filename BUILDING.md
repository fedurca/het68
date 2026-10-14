# Build (vendored Pico SDK)

This project uses a local Pico SDK clone at `./pico-sdk` (relative to
`CMakeLists.txt`; the directory is gitignored). No environment variables are
required after the SDK is present.

## Fetch the Pico SDK (first time / CI)

```bash
./scripts/fetch_pico_sdk.sh
```

This checks out **Pico SDK 2.2.0** (TinyUSB 0.18.0) and initializes the
`lib/tinyusb` submodule. TinyUSB patches are applied automatically by
`./build.sh` via `patches/apply_all.py`.

## Recommended

```bash
./build.sh
```

`./build.sh` builds only the **Pimoroni Pico Plus 2 W**
(`PICO_BOARD=pimoroni_pico_plus2_w_rp2350`: RP2350B, 16 MB flash, 8 MB PSRAM,
CYW43439 Wi-Fi/BT) and produces `build/pico_6mic_soundcard.uf2` and `.elf`.
Any other `PICO_BOARD` is rejected.

Pass `HET68_USB_DIAG=ON ./build.sh` to build the diagnostic firmware (simulated
1 kHz tone, no I2S).

## GitHub Releases (CI)

Publishing a GitHub Release with a SemVer core tag (`x.y.z` or `vx.y.z`, see
[semver.org](https://semver.org/)) triggers
[`.github/workflows/release.yml`](.github/workflows/release.yml): it fetches the
SDK, builds **Pico Plus 2 W** only, and uploads `.uf2` / `.elf` assets to that release (then moves the floating git
tag `latest`).

- Releases page: https://github.com/fedurca/het68/releases
- Example tags: `1.0.0`, `v1.0.0` (pre-release suffixes like `-rc.1` are not
  accepted by the release workflow)

## Manual CMake (Unix Makefiles)

```bash
rm -rf build
cmake -S . -B build -G "Unix Makefiles" -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
cmake --build build -j
```

## Manual CMake (Ninja, optional)

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
cmake --build build -j
```
