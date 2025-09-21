#!/bin/bash

# Tento skript připraví prostředí pro vývoj a spustí VS Code.
# Může být spuštěn dvěma způsoby:
#
# 1. `./setenv.sh` (doporučeno pro běžnou práci)
#    - Nastaví PICO_SDK_PATH pouze pro VS Code a spustí ho.
#    - Proměnná NEZŮSTANE nastavena ve vašem terminálu po skončení skriptu.
#
# 2. `source setenv.sh` (pokud potřebujete proměnnou i v terminálu)
#    - Nastaví PICO_SDK_PATH pro vaši AKTUÁLNÍ session v terminálu.
#    - Spustí VS Code. Proměnná PICO_SDK_PATH zůstane dostupná v terminálu.

echo "--- Nastavuji prostředí pro Pico vývoj ---"

# 1. Získání absolutní cesty k adresáři, kde se skript nachází
# Toto je robustní způsob, jak zajistit, že cesta je vždy správná,
# bez ohledu na to, odkud skript spouštíte.
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PICO_SDK_DIR="$SCRIPT_DIR/pico-sdk"

# 2. Kontrola, zda existuje adresář pico-sdk
if [ ! -d "$PICO_SDK_DIR" ]; then
    echo "Chyba: Adresář 'pico-sdk' nebyl nalezen v adresáři projektu."
    echo "Prosím, ujistěte se, že jste naklonovali pico-sdk sem: $SCRIPT_DIR"
    # Pokud je skript "sourcován", nemůžeme použít exit, použijeme return.
    (return 2>/dev/null) && return 1 || exit 1
fi

# 3. Nastavení cesty k Pico SDK
export PICO_SDK_PATH="$PICO_SDK_DIR"
echo "PICO_SDK_PATH nastaveno na: $PICO_SDK_PATH"

# 4. Inicializace/aktualizace submodulů v SDK (důležité pro TinyUSB atd.)
echo "Aktualizuji submoduly v pico-sdk (může chvíli trvat)..."
cd "$PICO_SDK_PATH"
# Přesměrování výstupu, aby se nezobrazovaly zbytečné informace
git submodule update --init > /dev/null 2>&1
cd "$SCRIPT_DIR"
echo "Submoduly jsou aktuální."

# 5. Spuštění VS Code
echo "Spouštím Visual Studio Code..."
code .

# 6. Informační zpráva
# Zjištění, zda byl skript spuštěn pomocí 'source'
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "--- Prostředí je připraveno pro VS Code. ---"
    echo "Poznámka: Proměnná PICO_SDK_PATH není nastavena ve vašem terminálu."
    echo "           Pro nastavení i v terminálu spusťte skript pomocí: source setenv.sh"
else
    echo "--- Prostředí je připraveno pro VS Code a váš terminál. ---"
fi

