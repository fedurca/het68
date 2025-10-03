rm -rf build
cmake -S . -B build -DPICO_BOARD=pico2      # use pico2_w for Pico 2 W
cmake --build build -j
