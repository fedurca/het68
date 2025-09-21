# Build (vendored Pico SDK)

This project uses the vendored Pico SDK located at `./pico-sdk` (relative to `CMakeLists.txt`).
No environment variables are required.

## Makefiles (macOS 26 / Apple Silicon)
```zsh
rm -rf build
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j
```
## Ninja (optional)
```zsh
brew install ninja
cmake -S . -B build -G Ninja
cmake --build build -j
```
