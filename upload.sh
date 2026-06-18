#!/bin/bash


# Flash the firmware using OpenOCD
/usr/local/bin/openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "program build/pico_6mic_soundcard.elf verify reset exit"

