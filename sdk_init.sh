cd ~/het68

rm -rf pico-sdk

git clone https://github.com/raspberrypi/pico-sdk.git pico-sdk
cd pico-sdk
git submodule update --init lib/tinyusb
cd ..

# Apply required TinyUSB patch (PR #2937 backport).
# pico-sdk 0.18.0 (Dec 2024) is missing the fix that prevents SET_INTERFACE
# from timing out when streaming restarts.  This one-liner applies it:
patch -p1 -d pico-sdk/lib/tinyusb \
    < patches/tinyusb-0.18.0-pr2937-iso-activate.patch

./build.sh
