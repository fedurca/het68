#!/bin/bash

# Build the firmware
./build.sh

# Flash the firmware using OpenOCD
openocd -f interface/raspberrypi2-native.cfg -c "program build/pico_6mic_soundcard.uf2 verify reset exit"
