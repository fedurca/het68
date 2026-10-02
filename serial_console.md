# Sériová konzole a výpis běhu

Dvě různé konzole. Řádky `[VU 2kHz]` tiskne program spectro6 na počítači
(`het68_webflasher/spectro6/app.py`). Příkazy níže bere firmware na UART
(Debug Probe na GP0/GP1, nebo USB CDC port Pica). Oba vstupy vedou do stejného
CLI.

## Výpis `[VU 2kHz]`

Slovo `idle` na tom řádku znamená, že smyčka na počítači v tom tiku ještě
nesložila nový sloupec spektra. Mikrofony v tu chvíli jedou dál a čísla
v decibelech jsou poslední spočítaná úroveň.

Řádek se tiskne nejvýš čtyřikrát za sekundu. Na `idle` řádku se hladina drží
z minulého výpočtu a špička se netiskne. Na `live` řádku je nový průměr pásma
2 kHz ± 100 Hz a špička celého signálu od minulého `live` řádku.

| Položka | Význam |
|---|---|
| `mic1`…`mic6` | Hladina pásma 2 kHz ± 100 Hz, dB pod plnou úrovní |
| `peak` | Nejvyšší vzorek od minulého `live` řádku, celé pásmo |
| `idle` / `live` | V tom tiku nebyl / byl nový sloupec spektra |
| `frames` | Přijaté vzorky od startu nahrávání |
| `blocks` | Callbacky PortAudio, po 2048 vzorcích |
| `ovf` | Kolikrát host nestihl číst vstup |
| `drop` | Kolikrát byl kruhový buffer plný a blok se zahodil |
| `zeros_total` | Bloky, jejichž nejhlasitější vzorek byl pod 10⁻⁷ |

Zdravý běh drží `frames` a `blocks` v kroku 2048 vzorků na blok a má
`ovf=0`, `drop=0`, `zeros_total=0`. To znamená, že počítač stíhal, kruhový
buffer nic nezahodil a nepřišel blok digitálního ticha.

V klidu je pásmo 2 kHz typicky kolem −94 až −105 dB. Nejvýš bývá mic4, nejníž
mic1, rozdíl asi 6–8 dB. Špička v klidu bývá kolem −46 až −51 dB: ty rázy jsou
mimo pásmo 2 kHz. Hlasitější událost zvedne všech šest kanálů naráz. Špička
−240 dB je prázdný měřič (žádný vzorek nad 10⁻⁷ se do něj v tom okně nedostal),
ne výpadek proudu, pokud `frames` dál rostou a `zeros_total` zůstává 0.

## Sériové číslo

První řádek po startu a po otevření USB CDC končí `serial=` a 16 hexadecimálními
znaky. Je to totéž číslo, které Linux vypíše jako `SerialNumber` v dmesg:
identifikátor z OTP čipu RP2350, ne pevný text. `STATUS` ho vypíše znovu na
vlastním řádku.

## Proč ve výpisu chybí čas

Řádek `[VU 2kHz]` hodiny nemá. Čas je jen na UART srdci Pica: `[123s]` jsou
sekundy od startu čipu a na konci řádku je `time=ok` nebo `time=unsynced`.
`time=ok` znamená, že přišel `TIME SYNC` s unixovými sekundami. Kalendářní
čas se na ten řádek nepíše.

## Mikrofony

Pozorovatel stojí čelem k severu, mic1 míří pryč od něj. Zkratky jsou jména
z USB mapy kanálů (`TFC TRR TSL BC RLC RRC`). Pořadí vzorků je pořád
kanál 1 = mic1.

| Mic | Zkratka | Od pozorovatele |
|---|---|---|
| mic1 | TFC | horní, přímo před ním, sever |
| mic2 | TRR | horní, vpravo a dozadu, azimut 120° |
| mic3 | TSL | horní, vlevo a dozadu, azimut 240° |
| mic4 | BC | dolní, za ním, jih |
| mic5 | RLC | dolní, vlevo a dopředu, azimut 300° |
| mic6 | RRC | dolní, vpravo a dopředu, azimut 60° |

`TRL` by mic3 popsalo přesněji, ale jeho bit je nižší než `TRR`, takže by ho
Linux přiřadil kanálu 2. Dolní přední levý a dolní přední pravý ve standardu
UAC2 nejsou, proto jsou kanály 5 a 6 `RLC` a `RRC`.

## Příkazy UART

Příkaz se odešle Enterem (CR nebo LF). Po restartu jsou srdce i log zapnuté.
Časové značky detekcí platí až po `TIME SYNC`.

| Příkaz | Co udělá |
|---|---|
| `HELP` nebo `?` | vypíše nápovědu a verzi |
| `STATUS` | hodiny, počty detekcí, entity, drony, RID, log, rychlost zvuku, barometr, link |
| `TIME` | stav hodin |
| `TIME INFO` | zdroj, stáří a kvalita synchronizace |
| `TIME SYNC <unix_s>` | nastaví čas; sekundy ≥ 1700000000, odpověď `TIME OK` |
| `LOG` / `LOG ON` / `LOG OFF` | stav nebo zapnutí výpisu `SRC` a `CMP` |
| `HB` / `HB ON` / `HB OFF` | stav nebo zapnutí řádku srdce |
| `DET LIST` | uložené detekce |
| `DET EXPORT` | detekce jako JSON řádky `NVREVT` |
| `DET BACKUP` | detekce jako hex `DETBLOB` |
| `DET IMPORT` | příjem `DETHEX` řádků, konec `DET END` |
| `DET DEL <id>` | smaže jednu detekci |
| `DET CLEAR` | smaže všechny detekce |
| `ENT LIST` | galerie šablon |
| `ENT EXPORT` / `ENT IMPORT` | galerie jako hex, konec importu `ENT END` |
| `DRONE LIST` | drony uložené ve flash |
| `DRONE CLEAR` | smaže ten seznam |
| `BARO` | tlak a teplota z DPS310 |
| `BARO RH` | ukáže předpokládanou vlhkost |
| `BARO RH <0-100>` | nastaví vlhkost pro rychlost zvuku |
| `LINK` | stav akustické linky a tabulka sousedů |
| `LINK ID <0-7>` | číslo uzlu a CDMA kód |
| `LINK BEACON` | vyšle jeden beacon |
| `LINK WIFI` | vyšle `WIFI_WAKE` |
| `RID LIST` | živé OpenDroneID stopy |
| `RID ON` / `RID OFF` | sken BLE Remote ID, jen deska s Wi-Fi |

## Položky srdce

Srdce jde třikrát za sekundu, když je `HB ON`.

| Položka | Význam |
|---|---|
| `[Ns]` | sekundy od startu čipu |
| `hb` | pořadové číslo srdce |
| `mnt` / `alt` | USB zvuk je připojený / číslo altsettingu |
| `dma ok miss hold late ph pk stall` | zdraví I2S a DMA |
| `clk freq itf` | host si vyžádal hodiny, kmitočet a rozhraní |
| `usb` | odeslané USB audio snímky |
| `bcn` | vyslané akustické beacony |
| `doa(out/act/iter)` | vydané směry / aktivní okna / iterace |
| `drone entity det rid` | počty dronu, entity, detekcí a RID stop |
| `time=ok` / `time=unsynced` | unixový čas je / není nastavený |
| `baro` | tlak, jen když DPS310 odpovídá |
| `flash=dirty` / `flash=saving` | čeká zápis do flash / zápis běží |
| `log=off` | výpis `SRC` je vypnutý |
