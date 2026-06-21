# Build (vendored Pico SDK)

This project uses the vendored Pico SDK located at `./pico-sdk` (relative to
`CMakeLists.txt`). No environment variables are required.

## Recommended

```bash
./build.sh
```

`build.sh` configures and builds for `PICO_BOARD=pico2` (RP2350) and produces
`build/pico_6mic_soundcard.uf2` and `.elf`. Pass `HET68_USB_DIAG=ON ./build.sh`
to build the diagnostic firmware (simulated 1 kHz tone, no I2S).

## Manual CMake (Unix Makefiles)

```bash
rm -rf build
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j
```

## Manual CMake (Ninja, optional)

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
```
