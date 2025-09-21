#!/bin/bash

echo "--- Kontrola prostředí ---"

# Pokusí se najít adresář s kompilátorem ARM GCC
TOOLCHAIN_BIN_DIR=""

# 1. Zkusí standardní systémovou cestu (PATH), která by nyní měla fungovat
if command -v arm-none-eabi-gcc &> /dev/null; then
    TOOLCHAIN_BIN_DIR=$(dirname "$(command -v arm-none-eabi-gcc)")
    echo "Kompilátor nalezen v systémové cestě (PATH)."
# 2. Pokud selže, zkusí novou standardní cestu pro macOS instalátor, kterou jsme objevili
elif [ -f "/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi/bin/arm-none-eabi-gcc" ]; then
    TOOLCHAIN_BIN_DIR="/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi/bin"
    echo "Kompilátor nalezen ve standardní složce /Applications."
    # Dočasně přidá cestu do PATH, aby ji našly i podprocesy
    export PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi

# Pokud nebyl nalezen ani na jednom místě, skončí s chybou
if [ -z "$TOOLCHAIN_BIN_DIR" ]; then
    echo "--- CHYBA: Kompilátor 'arm-none-eabi-gcc' nebyl nalezen. ---"
    echo "Prosím, ujistěte se, že je ARM GNU Toolchain správně nainstalován."
    echo "Odkaz ke stažení: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
    exit 1
fi

# --- Nastavení prostředí ---
echo "--- Nastavuji prostředí pro Pico vývoj ---"

# 1. Nastavení cesty k Pico SDK relativně ke skriptu
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)
export PICO_SDK_PATH="${SCRIPT_DIR}/pico-sdk"
echo "PICO_SDK_PATH nastaveno na: ${PICO_SDK_PATH}"

# 2. Nastavení cesty ke kompilátoru, kterou CMake použije
export PICO_TOOLCHAIN_PATH="$TOOLCHAIN_BIN_DIR"
echo "PICO_TOOLCHAIN_PATH nastaveno na: ${PICO_TOOLCHAIN_PATH}"

# Aktualizace submodulů v SDK
echo "Aktualizuji submoduly v pico-sdk (může chvíli trvat)..."
cd "${PICO_SDK_PATH}"
git submodule update --init --force > /dev/null 2>&1
cd "${SCRIPT_DIR}"
echo "Submoduly jsou aktuální."

# Spuštění VS Code v aktuálním adresáři
echo "Spouštím Visual Studio Code..."
code .

echo "--- Prostředí je připraveno. ---"


