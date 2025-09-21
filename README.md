6-kanálová I2S USB Zvuková karta pro Raspberry Pi Pico
Tento projekt promění Raspberry Pi Pico v 6-kanálové USB audio zařízení (mikrofon), které přijímá data ze tří stereo I2S datových linek. Je navržen pro použití se šesti mikrofony ICS43434, zapojenými v párech.

Projekt využívá PIO k implementaci I2S přijímačů a DMA k přenosu dat z PIO do paměti bez zatížení procesoru. Následně jsou data odesílána přes USB do hostitelského počítače pomocí knihovny TinyUSB.

Hardwarové zapojení
POZOR: Napájení mikrofonů musí být připojeno k 3V3 (OUT), nikoliv k VSYS, aby nedošlo k jejich zničení!

Napájení:

VDD všech 6 mikrofonů -> 3V3 (OUT) na Pico (Pin 36)

GND všech 6 mikrofonů -> GND na Pico (např. Pin 38)

Společné hodinové signály (Pico je Master):

GP0 -> WS (Word Select) na všech 6 mikrofonech

GP1 -> SCK (Serial Clock) na všech 6 mikrofonech

Datové linky (3 stereo páry):

Kanál 1 & 2 (na GP2):

Mikrofon 1: SEL na GND (levý kanál)

Mikrofon 2: SEL na 3V3 (pravý kanál)

Spojené SD výstupy -> GP2 na Pico

Kanál 3 & 4 (na GP3):

Mikrofon 3: SEL na GND (levý kanál)

Mikrofon 4: SEL na 3V3 (pravý kanál)

Spojené SD výstupy -> GP3 na Pico

Kanál 5 & 6 (na GP4):

Mikrofon 5: SEL na GND (levý kanál)

Mikrofon 6: SEL na 3V3 (pravý kanál)

Spojené SD výstupy -> GP4 na Pico

Nastavení vývojového prostředí
Nainstalujte nástroje: Ujistěte se, že máte nainstalovaný arm-none-eabi-gcc (kompilátor), CMake, a make.

Stáhněte Pico SDK: Naklonujte oficiální pico-sdk a nastavte cestu k němu v proměnné prostředí PICO_SDK_PATH.

git clone -b master [https://github.com/raspberrypi/pico-sdk.git](https://github.com/raspberrypi/pico-sdk.git)
cd pico-sdk
git submodule update --init
export PICO_SDK_PATH=$(pwd)

Otevřete projekt ve VS Code: Otevřete tuto složku ve VS Code.

Nainstalujte doporučená rozšíření: VS Code vám nabídne instalaci rozšíření "CMake Tools" a "Cortex-Debug". Nainstalujte je.

Konfigurace CMake: Při prvním otevření CMake Tools automaticky nakonfiguruje projekt. Vyberte "GCC for arm-none-eabi".

Sestavení a nahrání
Sestavení (Build):

Stiskněte F7 nebo klikněte na tlačítko "Build" ve spodní liště VS Code.

Projekt se zkompiluje a v adresáři build vznikne soubor pico_6mic_soundcard.uf2.

Nahrání (Upload):

Připojte Pico k počítači zatímco držíte tlačítko BOOTSEL.

Pico se objeví jako nový disk RPI-RP2.

Přetáhněte soubor pico_6mic_soundcard.uf2 na tento disk.

Pico se automaticky restartuje.

Použití
Po restartu se Pico v operačním systému (Windows, macOS, Linux) přihlásí jako standardní 6-kanálové USB audio zařízení. Můžete jej vybrat jako vstupní zařízení v jakémkoliv audio softwaru (např. Audacity, Reaper, OBS) a nahrávat ze všech šesti mikrofonů současně.

Poznámka k RP2350: Tento kód je napsán pro standardní Pico s čipem RP2040. Pro novější Pico s RP2350 mohou být nutné drobné úpravy v konfiguraci SDK, ale PIO a DMA kód by měl zůstat funkční.