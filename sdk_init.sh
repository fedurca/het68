cd ~/het68

rm -rf pico-sdk

git clone https://github.com/raspberrypi/pico-sdk.git pico-sdk
cd pico-sdk
git submodule update --init lib/tinyusb
cd ..

# Apply TinyUSB backport (3 fixes in one patch file).
# pico-sdk 0.18.0 bundles TinyUSB without any of these fixes:
#   Fix 1 (PR #2937)  — dcd_rp2040.c: abort active ISO transfer in iso_activate()
#   Fix 2 (4bfba6b)   — dcd_rp2040.c: reset ep state BEFORE notifying stack
#   Fix 3 (new)       — rp2040_usb.c: ISO ZLP buf_status must not panic
patch -p1 -d pico-sdk/lib/tinyusb \
    < patches/tinyusb-0.18.0-pr2937-iso-activate.patch

# Verify all three fixes landed.
dcd=pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c
usb=pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
fix1=$(grep -c "hw_endpoint_abort_xfer" "$dcd")
fix2=$(grep -c "xferred_len = ep->xferred_len" "$dcd")
fix3=$(grep -c "TUSB_XFER_ISOCHRONOUS" "$usb")
if [ "$fix1" -eq 3 ] && [ "$fix2" -eq 1 ] && [ "$fix3" -eq 1 ]; then
    echo "✓ Všechny 3 TinyUSB patche aplikovány OK"
else
    echo "✗ CHYBA: patch se neaplikoval správně (fix1=$fix1 fix2=$fix2 fix3=$fix3)"
    exit 1
fi

./build.sh
