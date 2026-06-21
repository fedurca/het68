# PR: audio: accept class request wIndex addressed to an AS interface

**Target:** `hathach/tinyusb` (master, TinyUSB 0.20.1-dev)
**Branch:** `rp2350-uac2-entity-as-interface`
**File:** `src/class/audio/audio_device.c` — `audiod_verify_entity_exists()`

## Problem

`audiod_verify_entity_exists()` matches the request `wIndex` only against the
function's **standard Audio Control (AC)** interface number:

```c
if (_audiod_fct[i].p_desc &&
    ((tusb_desc_interface_t const *) _audiod_fct[i].p_desc)->bInterfaceNumber == itf) {
```

Some hosts (notably **Linux / ALSA**) address class-specific entity requests
(e.g. `CLOCK_SOURCE` `GET_CUR SAM_FREQ` / `CLK_VALID`) using one of the
function's **Audio Streaming (AS)** interface numbers instead of the AC
interface. Those requests fail entity verification and the control transfer is
rejected.

On **RP2350** this is fatal: the host's clock-validity query times out
(`err -110`) and the isochronous IN stream never starts.

## Fix

Accept the request if `wIndex` refers to the AC interface (original behaviour)
**or** to any interface descriptor inside the function's own descriptor block.
The scan is bounded by `desc_length` and mirrors the existing
`audiod_verify_itf_exists()` pattern, so it cannot read out of bounds and never
rejects a request the original code accepted.

## Risk / compatibility

- Additive: the original AC-interface match is still tried first.
- No new OOB reads (bounded by `desc_length`).
- No ABI / public API change.

## Testing

- Verified against an RP2350 (Pico 2) 6-channel UAC2 capture device: Linux host
  now completes clock-validity and starts the ISO stream reliably.
- **Maintainer note:** this was developed against a vendored TinyUSB 0.18 tree
  and forward-ported to current `master`; please run CI / your UAC2 test matrix.

## Related RP2350 findings (not in this PR — need 0.20 hardware validation)

These are documented for visibility; happy to open follow-up PRs/issues:

1. **RP2350 `hw_endpoint_abort_xfer` spin.** `src/portable/raspberrypi/rp2040/dcd_rp2040.c`
   still does `while ((usb_hw->abort_done & abort_mask) != abort_mask) {}` under
   `rp2040_chip_version() >= 2`. On RP2350 this can spin forever and wedge the
   USB ISR.
2. **Deterministic first ISO IN.** First IN after `SET_INTERFACE` alt=1 needs a
   full-frame (or carefully ordered) start; an inline ZLP before status can race
   on RP2350.
