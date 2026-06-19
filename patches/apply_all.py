#!/usr/bin/env python3
"""Apply all TinyUSB patches + debug checkpoints to pico-sdk.  Idempotent.

Usage (from project root):
    python3 patches/apply_all.py

Patches applied:
  dcd_rp2040.c  Fix 1a hw_endpoint_abort_xfer()
                Fix 1b use abort_xfer in reset_ep0()
                Fix 1c dcd_edpt_iso_activate: abort before re-enable
                Fix 2  reset ep state before notifying stack (4bfba6b)
  rp2040_usb.c  Fix 3  ISO ZLP buf_status: return false (not panic)
                Fix 4  NULL-guard ep->endpoint_control (RP2350 bug)
                CP-I/J hw_endpoint_start_next_buffer checkpoints
  audio_device.c CP-O/P checkpoints after set_itf_cb / before tud_control_status
  usbd_control.c CP-Q/R checkpoints in tud_control_status
  usbd.c         CP-K/L checkpoints in usbd_edpt_xfer
"""
import sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DCD  = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c")
USB  = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c")
AC   = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/class/audio/audio_device.c")
UC   = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/device/usbd_control.c")
UD   = os.path.join(ROOT, "pico-sdk/lib/tinyusb/src/device/usbd.c")

EXTERN_DECLS = (
    'extern void dbg_putc(char c); extern void dbg_puts(const char *s);\n'
    '#define DBG_CP(c) do{dbg_putc(c);dbg_putc(\'\\n\');}while(0)\n'
)


def apply(path, old, new, name):
    with open(path) as f: t = f.read()
    if old in t:
        with open(path, 'w') as f: f.write(t.replace(old, new, 1))
        print(f"  ✓ {name}")
        return True
    elif new.strip()[:40] in t:
        print(f"  = {name}  [already]")
        return False
    else:
        print(f"  ✗ {name}  [pattern not found]", file=sys.stderr)
        return False


def add_extern(path):
    """Prepend extern declarations if not already present."""
    with open(path) as f: t = f.read()
    if 'extern void dbg_putc' not in t:
        with open(path, 'w') as f: f.write(EXTERN_DECLS + t)
        print(f"  ✓ extern decls → {os.path.basename(path)}")
    else:
        print(f"  = extern decls already in {os.path.basename(path)}")


def main():
    os.chdir(ROOT)
    for p in [DCD, USB, AC, UC, UD]:
        if not os.path.exists(p):
            sys.exit(f"ERROR: {p} not found – clone pico-sdk first (sdk_init.sh)")

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(DCD)}")

    apply(DCD,
        """\
static void hw_endpoint_xfer(uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
  struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_addr);
  hw_endpoint_xfer_start(ep, buffer, total_bytes);
}

static void __tusb_irq_path_func(hw_handle_buff_status)(void) {""",
        """\
static void hw_endpoint_xfer(uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
  struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_addr);
  hw_endpoint_xfer_start(ep, buffer, total_bytes);
}

// Abort a pending transfer.  EP_ABORT hardware spin (abort_done) is skipped:
// on RP2350 it never completes, freezing the USB ISR.
static void hw_endpoint_abort_xfer(struct hw_endpoint* ep) {
  uint32_t buf_ctrl = USB_BUF_CTRL_SEL;
  if (ep->next_pid) buf_ctrl |= USB_BUF_CTRL_DATA1_PID;
  _hw_endpoint_buffer_control_set_value32(ep, buf_ctrl);
  hw_endpoint_reset_transfer(ep);
}

static void __tusb_irq_path_func(hw_handle_buff_status)(void) {""",
        "Fix 1a – add hw_endpoint_abort_xfer()")

    apply(DCD,
        """\
      if (done) {
        // Notify
        dcd_event_xfer_complete(0, ep->ep_addr, ep->xferred_len, XFER_RESULT_SUCCESS, true);
        hw_endpoint_reset_transfer(ep);""",
        """\
      if (done) {
        uint16_t const xferred_len = ep->xferred_len;
        hw_endpoint_reset_transfer(ep);
        dcd_event_xfer_complete(0, ep->ep_addr, xferred_len, XFER_RESULT_SUCCESS, true);""",
        "Fix 2  – reset ep state before notify (4bfba6b)")

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
      hw_endpoint_abort_xfer(ep);
    }""",
        "Fix 1b – use abort_xfer in reset_ep0()")

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
  TU_ASSERT(ep->hw_data_buf != NULL);
  if (ep->active) { hw_endpoint_abort_xfer(ep); }
  ep->wMaxPacketSize = ep_desc->wMaxPacketSize;
  hw_endpoint_enable(ep);
  return true;
}""",
        "Fix 1c – dcd_edpt_iso_activate abort before re-enable")

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(USB)}")

    apply(USB,
        """\
  if (!ep->active) {
    panic("Can't continue xfer on inactive ep %02X", ep->ep_addr);
  }""",
        """\
  if (!ep->active) {
    if (ep->transfer_type == TUSB_XFER_ISOCHRONOUS) {
      hw_endpoint_lock_update(ep, -1);
      return false;
    }
    panic("Can't continue xfer on inactive ep %02X", ep->ep_addr);
  }""",
        "Fix 3  – ISO ZLP buf_status return false")

    apply(USB,
        "  *ep->endpoint_control = ep_ctrl;",
        "  if (ep->endpoint_control) *ep->endpoint_control = ep_ctrl;  // EP0=NULL on RP2350",
        "Fix 4  – NULL guard ep->endpoint_control")

    # CP-I/J: checkpoints around _hw_endpoint_buffer_control_set_value32
    add_extern(USB)
    apply(USB,
        "  _hw_endpoint_buffer_control_set_value32(ep, buf_ctrl);\n}",
        "  DBG_CP('I');\n  _hw_endpoint_buffer_control_set_value32(ep, buf_ctrl);\n  DBG_CP('J');\n}",
        "CP-I/J – hw_endpoint_start_next_buffer")

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(AC)}")
    add_extern(AC)

    # Fix 5a: clear the busy flag so audiod_tx_done_cb can start a new transfer.
    # usbd_edpt_clear_stall only clears busy when stalled; usbd_edpt_release
    # always clears it.
    apply(AC,
        "          usbd_edpt_clear_stall(rhport, ep_addr);",
        ("          usbd_edpt_clear_stall(rhport, ep_addr);\n"
         "          usbd_edpt_release(rhport, ep_addr); // Fix 5a: clear busy after ISO abort"),
        "Fix 5a – usbd_edpt_release after clear_stall to reset busy flag")

    # Fix 5b: TU_VERIFY(audiod_tx_done_cb(...)) with -O3 generates UB: 'return;'
    # in a bool function.  GCC may not actually return, causing fall-through with
    # corrupted state.  Remove TU_VERIFY so a failed call is silently ignored —
    # the main loop will start normal streaming at the next xfer_complete event.
    apply(AC,
        "            TU_VERIFY(audiod_tx_done_cb(rhport, &_audiod_fct[func_id]));",
        ("            audiod_tx_done_cb(rhport, &_audiod_fct[func_id]); // Fix 5b: no TU_VERIFY UB"),
        "Fix 5b – remove TU_VERIFY from audiod_tx_done_cb call (UB with -O3)")

    apply(AC,
        "      TU_VERIFY(tud_audio_set_itf_cb(rhport, p_request));",
        "      TU_VERIFY(tud_audio_set_itf_cb(rhport, p_request));\n      DBG_CP('O');",
        "CP-O – after set_itf_cb in audiod_set_interface")

    apply(AC,
        "  tud_control_status(rhport, p_request);",
        "  DBG_CP('P');\n  tud_control_status(rhport, p_request);",
        "CP-P – before tud_control_status in audiod_set_interface")

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(UC)}")
    add_extern(UC)

    apply(UC,
        "  return status_stage_xact(rhport, request);",
        ("  DBG_CP('Q');\n"
         "  bool _ssr = status_stage_xact(rhport, request);\n"
         "  DBG_CP('R');\n"
         "  return _ssr;"),
        "CP-Q/R – tud_control_status")

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(UD)}")
    add_extern(UD)

    apply(UD,
        "  TU_ASSERT(_usbd_dev.ep_status[epnum][dir].busy == 0);\n\n  // Set busy",
        ("  DBG_CP('K'); dbg_putc('0'+epnum); dbg_putc('0'+dir);\n"
         "  dbg_putc('0'+_usbd_dev.ep_status[epnum][dir].busy); dbg_putc('\\n');\n"
         "  TU_ASSERT(_usbd_dev.ep_status[epnum][dir].busy == 0);\n"
         "  DBG_CP('L');\n\n  // Set busy"),
        "CP-K/L – usbd_edpt_xfer entry")

    # ── Summary ──────────────────────────────────────────────────────────────
    print("\n── Verify:")
    checks = [
        (DCD, "hw_endpoint_abort_xfer",     "3"),
        (USB, "TUSB_XFER_ISOCHRONOUS",      "1"),
        (USB, "ep->endpoint_control)",      "1"),
        (USB, "DBG_CP('I')",               "1"),
        (AC,  "usbd_edpt_release",          "1"),
        (AC,  "Fix 5b",                     "1"),
        (AC,  "DBG_CP('O')",               "1"),
        (UC,  "DBG_CP('Q')",               "1"),
        (UD,  "DBG_CP('K')",               "1"),
    ]
    all_ok = True
    for path, pattern, expect in checks:
        with open(path) as f: t = f.read()
        n = t.count(pattern)
        ok = n == int(expect)
        all_ok = all_ok and ok
        print(f"  {'✓' if ok else '✗'} {os.path.basename(path)}: '{pattern}' ×{n}  (expect {expect})")

    if all_ok:
        print("\n✓ All patches OK → ./build.sh && ./upload.sh")
    else:
        print("\n✗ Some patches missing – check errors above", file=sys.stderr)


if __name__ == "__main__":
    main()
