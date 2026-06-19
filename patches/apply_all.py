#!/usr/bin/env python3
"""Apply all TinyUSB patches to pico-sdk.  Idempotent — safe to run twice.

Usage (from project root):
    python3 patches/apply_all.py
"""
import sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DCD  = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c")
USB  = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c")


def apply(path, old, new, name):
    with open(path) as f: t = f.read()
    if old in t:
        with open(path,'w') as f: f.write(t.replace(old, new, 1))
        print(f"  ✓ {name}")
        return True
    elif new.strip()[:40] in t:
        print(f"  = {name}  [already applied]")
        return False
    else:
        print(f"  ✗ {name}  [OLD PATTERN NOT FOUND – file may be in unexpected state]", file=sys.stderr)
        return False


def main():
    os.chdir(ROOT)
    for p in [DCD, USB]:
        if not os.path.exists(p):
            sys.exit(f"ERROR: {p} not found – clone pico-sdk first (sdk_init.sh)")

    print(f"\n── {os.path.basename(DCD)}")

    # ── FIX 1a  add hw_endpoint_abort_xfer() ──────────────────────────────
    apply(DCD,
        # old (original TinyUSB 0.18.0, before any of our patches)
        """\
static void hw_endpoint_xfer(uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
  struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_addr);
  hw_endpoint_xfer_start(ep, buffer, total_bytes);
}

static void __tusb_irq_path_func(hw_handle_buff_status)(void) {""",
        # new
        """\
static void hw_endpoint_xfer(uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
  struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_addr);
  hw_endpoint_xfer_start(ep, buffer, total_bytes);
}

// Abort a pending transfer.  EP_ABORT hardware spin (abort_done register) is
// intentionally skipped: on RP2350 it never completes, freezing the USB ISR.
static void hw_endpoint_abort_xfer(struct hw_endpoint* ep) {
  uint32_t buf_ctrl = USB_BUF_CTRL_SEL;
  if (ep->next_pid) buf_ctrl |= USB_BUF_CTRL_DATA1_PID;
  _hw_endpoint_buffer_control_set_value32(ep, buf_ctrl);
  hw_endpoint_reset_transfer(ep);
}

static void __tusb_irq_path_func(hw_handle_buff_status)(void) {""",
        "Fix 1a – add hw_endpoint_abort_xfer()")

    # ── FIX 2   reset ep state BEFORE notifying stack ─────────────────────
    apply(DCD,
        """\
      if (done) {
        // Notify
        dcd_event_xfer_complete(0, ep->ep_addr, ep->xferred_len, XFER_RESULT_SUCCESS, true);
        hw_endpoint_reset_transfer(ep);""",
        """\
      if (done) {
        // Reset BEFORE notify (4bfba6b): audio driver queues the next ISO
        // transfer from the xfer-complete callback; ep->active must be false.
        uint16_t const xferred_len = ep->xferred_len;
        hw_endpoint_reset_transfer(ep);
        dcd_event_xfer_complete(0, ep->ep_addr, xferred_len, XFER_RESULT_SUCCESS, true);""",
        "Fix 2  – reset ep state before notifying stack (commit 4bfba6b)")

    # ── FIX 1b  use hw_endpoint_abort_xfer in reset_ep0() ─────────────────
    apply(DCD,
        """\
    if (ep->active) {
      // Abort any pending transfer from a prior control transfer per USB specs
      // Due to Errata RP2040-E2: ABORT flag is only applicable for B2 and later (unusable for B0, B1).
      // Which means we are not guaranteed to safely abort pending transfer on B0 and B1.
      uint32_t const abort_mask = (dir ? USB_EP_ABORT_EP0_IN_BITS : USB_EP_ABORT_EP0_OUT_BITS);
      if (rp2040_chip_version() >= 2) {
        usb_hw_set->abort = abort_mask;
        while ((usb_hw->abort_done & abort_mask) != abort_mask) {}
      }

      _hw_endpoint_buffer_control_set_value32(ep, USB_BUF_CTRL_DATA1_PID | USB_BUF_CTRL_SEL);
      hw_endpoint_reset_transfer(ep);

      if (rp2040_chip_version() >= 2) {
        usb_hw_clear->abort_done = abort_mask;
        usb_hw_clear->abort = abort_mask;
      }
    }
    ep->next_pid = 1u;""",
        """\
    ep->next_pid = 1u;
    if (ep->active) {
      hw_endpoint_abort_xfer(ep); // PR #2937 backport
    }""",
        "Fix 1b – use hw_endpoint_abort_xfer in reset_ep0()")

    # ── FIX 1c  dcd_edpt_iso_activate: abort before re-enable ─────────────
    apply(DCD,
        """\
bool dcd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const * ep_desc) {
  (void) rhport;
  const uint8_t ep_addr = ep_desc->bEndpointAddress;
  // Fill in endpoint control register with buffer offset
  struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_addr);
  TU_ASSERT(ep->hw_data_buf != NULL); // must be inited and buffer allocated
  ep->wMaxPacketSize = ep_desc->wMaxPacketSize;

  hw_endpoint_enable(ep);
  return true;
}""",
        """\
bool dcd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const * ep_desc) {
  (void) rhport;
  struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_desc->bEndpointAddress);
  TU_ASSERT(ep->hw_data_buf != NULL); // must be inited and allocated previously
  if (ep->active) {
    hw_endpoint_abort_xfer(ep); // abort any in-flight transfer (PR #2937)
  }
  ep->wMaxPacketSize = ep_desc->wMaxPacketSize;
  hw_endpoint_enable(ep);
  return true;
}""",
        "Fix 1c – dcd_edpt_iso_activate: abort before re-enable")

    print(f"\n── {os.path.basename(USB)}")

    # ── FIX 3   ISO ZLP buf_status: return false instead of panic ──────────
    apply(USB,
        """\
  if (!ep->active) {
    panic("Can't continue xfer on inactive ep %02X", ep->ep_addr);
  }""",
        """\
  if (!ep->active) {
    // Fix 3: RP2040/2350 fires buf_status for ISO IN on every SOF even when
    // AVAIL was not set (hardware sent ZLP).  Return false so the real
    // xfer-complete event handles the buffer reload; no spurious notify.
    if (ep->transfer_type == TUSB_XFER_ISOCHRONOUS) {
      hw_endpoint_lock_update(ep, -1);
      return false;
    }
    panic("Can't continue xfer on inactive ep %02X", ep->ep_addr);
  }""",
        "Fix 3  – ISO ZLP buf_status: return false instead of panic")

    # ── FIX 4   NULL guard for ep->endpoint_control (EP0, RP2350) ──────────
    apply(USB,
        "  *ep->endpoint_control = ep_ctrl;",
        "  if (ep->endpoint_control) *ep->endpoint_control = ep_ctrl;  // EP0=NULL on RP2350",
        "Fix 4  – NULL-guard ep->endpoint_control (RP2350 boot-ROM write fault)")

    print("\n── Verify:")
    for path, pattern, expect in [
        (DCD, "hw_endpoint_abort_xfer", "3"),
        (USB, "TUSB_XFER_ISOCHRONOUS",  "1"),
        (USB, "ep->endpoint_control)",  "1"),
    ]:
        with open(path) as f: t = f.read()
        n = t.count(pattern)
        ok = "✓" if str(n) == expect else "✗"
        print(f"  {ok} {os.path.basename(path)}: '{pattern}' × {n}  (expect {expect})")

    print("\nAll done.  Run: ./build.sh && ./upload.sh")


if __name__ == "__main__":
    main()
