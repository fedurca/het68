#!/bin/bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" -c "program build/pico_6mic_soundcard.elf verify reset exit"
