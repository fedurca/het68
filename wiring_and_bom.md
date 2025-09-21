Zapojení na RP2350

GP0 = WS modrý
GP1 = SCLK hnědý
GND = společná zem
GP2 = DOUT1 (mic 1+2)
GP3 = DOUT1 (mic 3+4)
GP4 = DOUT1 (mic 5+6)
3V - oranžový na VSYS

zapojení ICS-43434 breakout boardu
https://www.aliexpress.com/item/1005008956861273.html


černý (SEL): společný výběr kanálu  v našem případě:
1,3,5 na GND (levý kanál)
2,4,6 na 3V (pravý kanál) - oranžový Y klema

modrý (LRCL/WS): společný hodinový signál, který určuje, jestli se přenáší data pro levý nebo pravý kanál

zelený (DOUT): samostatný datový výstup
3 zelené vodiče, každý pro jeden pár mikrofonů
1+2
3+4
5+6

hnědý (SCLK): společný hodinový signál který synchronizuje přenos dat

černý (GND): společná zem.

oranžový (3V): společné napájení.

BME280 - teplota, tlak, vlhkost

INA219 - jednokanálový I2C napětí, proud

INA3221 - tříkanálový

HITPOINT PT-2038WQ - piezo bez budiče  
https://www.gme.cz/v/1496869/hitpoint-pt-2038wq-piezomenic



Sekce: macOS (Apple silicon) hosted cross toolchains
Podsekce: AArch32 bare-metal target (arm-none-eabi)
Soubor ke stažení: arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi.pkg
https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads

filename:
arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi.pkg
size: 
271.3MB
sha256sum:
b93712026cec9f98a5d98dfec84e8096d32be3759642381e1982c4a5d2aa020b 
md5sum: 
d2f3017bbf1e644b8a41b0bf499c0617  
URL:
https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi.pkg