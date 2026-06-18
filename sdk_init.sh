cd ~/het68

rm -rf pico-sdk

git clone https://github.com/raspberrypi/pico-sdk.git pico-sdk
cd pico-sdk
git submodule update --init
cd ..

./build.sh
