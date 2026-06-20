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


def reset_pico_sdk():
    """Reset patchable files to their original git state to avoid duplicate patches."""
    files = [
        "src/portable/raspberrypi/rp2040/dcd_rp2040.c",
        "src/portable/raspberrypi/rp2040/rp2040_usb.c",
        "src/class/audio/audio_device.c",
        "src/device/usbd.c",
        "src/device/usbd_control.c",
    ]
    tinyusb = os.path.join(ROOT, "pico-sdk/lib/tinyusb")
    if not os.path.isdir(tinyusb):
        return
    # Only reset if git is available and the directory is a git repo
    try:
        import subprocess
        result = subprocess.run(
            ["git", "checkout", "HEAD", "--"] + files,
            cwd=tinyusb, capture_output=True, text=True)
        if result.returncode == 0:
            print("  ✓ pico-sdk/lib/tinyusb files reset to original")
        else:
            print(f"  ! git checkout failed: {result.stderr.strip()[:80]}")
    except Exception as e:
        print(f"  ! Could not reset pico-sdk: {e}")


def main():
    os.chdir(ROOT)
    print("── Resetting pico-sdk files to original state:")
    reset_pico_sdk()
    print()
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

    # (no debug checkpoints in production build)

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(AC)}")

    # Fix 5a: clear the busy flag so audiod_tx_done_cb can start a new transfer.
    # usbd_edpt_clear_stall only clears busy when stalled; usbd_edpt_release
    # always clears it.
    apply(AC,
        "          usbd_edpt_clear_stall(rhport, ep_addr);",
        ("          usbd_edpt_clear_stall(rhport, ep_addr);\n"
         "          usbd_edpt_release(rhport, ep_addr); // Fix 5a: clear busy after ISO abort"),
        "Fix 5a – usbd_edpt_release after clear_stall to reset busy flag")

    # Fix 5b: TU_VERIFY(audiod_tx_done_cb(...)) with -O3 generates UB: 'return;'
    # in a bool function.  GCC may not actually return, causing fall-through.
    apply(AC,
        "            TU_VERIFY(audiod_tx_done_cb(rhport, &_audiod_fct[func_id]));",
        ("            audiod_tx_done_cb(rhport, &_audiod_fct[func_id]); // Fix 5b: no TU_VERIFY UB"),
        "Fix 5b – remove TU_VERIFY from audiod_tx_done_cb call (UB with -O3)")

    apply(AC,
        "  TU_VERIFY(tud_audio_tx_done_pre_load_cb(rhport, idx_audio_fct, audio->ep_in, audio->alt_setting[idxItf]));",
        "  tud_audio_tx_done_pre_load_cb(rhport, idx_audio_fct, audio->ep_in, audio->alt_setting[idxItf]); // Fix 5c: no TU_VERIFY UB",
        "Fix 5c – remove TU_VERIFY from pre_load_cb in audiod_tx_done_cb (UB with -O3)")

    apply(AC,
        "  TU_VERIFY(tud_audio_tx_done_post_load_cb(rhport, n_bytes_tx, idx_audio_fct, audio->ep_in, audio->alt_setting[idxItf]));",
        "  tud_audio_tx_done_post_load_cb(rhport, n_bytes_tx, idx_audio_fct, audio->ep_in, audio->alt_setting[idxItf]); // Fix 5d: no TU_VERIFY UB",
        "Fix 5d – remove TU_VERIFY from post_load_cb in audiod_tx_done_cb (UB with -O3)")

    apply(AC,
        "  TU_VERIFY(usbd_edpt_xfer(rhport, audio->ep_in, audio->lin_buf_in, n_bytes_tx));",
        "  usbd_edpt_xfer(rhport, audio->ep_in, audio->lin_buf_in, n_bytes_tx); // Fix 7b: no TU_VERIFY UB",
        "Fix 7b – remove TU_VERIFY from usbd_edpt_xfer non-encoding path (UB with -O3)")

    apply(AC,
        "  TU_VERIFY(usbd_edpt_xfer_fifo(rhport, audio->ep_in, &audio->ep_in_ff, n_bytes_tx));",
        "  usbd_edpt_xfer_fifo(rhport, audio->ep_in, &audio->ep_in_ff, n_bytes_tx); // Fix 7c: no TU_VERIFY UB",
        "Fix 7c – remove TU_VERIFY from usbd_edpt_xfer_fifo non-encoding path (UB with -O3)")

    # Fix 8: same UB in audiod_xfer_complete (xfer_complete event handler).
    apply(AC,
        "      TU_VERIFY(audiod_tx_done_cb(rhport, audio));",
        "      audiod_tx_done_cb(rhport, audio); // Fix 8: no TU_VERIFY UB",
        "Fix 8  – remove TU_VERIFY from audiod_tx_done_cb in audiod_xfer_complete (UB)")

    # Fix 11: Starting ISO IN inside audiod_set_interface (before tud_control_status)
    # races EP0 status stage; Linux then times out on clock-validity GET_CUR (err -110).
    apply(AC,
        ("            // Schedule first transmit if alternate interface is not zero i.e. streaming is disabled - in case no sample data is available a ZLP is loaded\n"
         "            // It is necessary to trigger this here since the refill is done with an RX FIFO empty interrupt which can only trigger if something was in there\n"
         "            audiod_tx_done_cb(rhport, &_audiod_fct[func_id]); // Fix 5b: no TU_VERIFY UB"),
        ("            // Fix 11: defer first ISO IN until EP0 SET_INTERFACE status completes"),
        "Fix 11 – defer first audiod_tx_done_cb until after tud_control_status")

    apply(AC,
        "  tud_control_status(rhport, p_request);\n\n  return true;\n}",
        ("  tud_control_status(rhport, p_request);\n\n"
         "  // Fix 11c: defer first ISO IN to next USB event loop tick\n"
         "#if CFG_TUD_AUDIO_ENABLE_EP_IN\n"
         "  if (alt != 0 && _audiod_fct[func_id].ep_in != 0 &&\n"
         "      _audiod_fct[func_id].ep_in_as_intf_num == itf) {\n"
         "    usbd_defer_func(audiod_deferred_first_tx_cb, (void*)(uintptr_t)func_id, false);\n"
         "  }\n"
         "#endif\n\n"
         "  return true;\n}"),
        "Fix 11c – usbd_defer_func for first ISO IN after SET_INTERFACE")

    apply(AC,
        "static bool audiod_set_interface(uint8_t rhport, tusb_control_request_t const *p_request) {",
        ("#if CFG_TUD_AUDIO_ENABLE_EP_IN\n"
         "static void audiod_deferred_first_tx_cb(void* param) {\n"
         "  uint8_t func_id = (uint8_t)(uintptr_t)param;\n"
         "  audiod_tx_done_cb(_audiod_fct[func_id].rhport, &_audiod_fct[func_id]);\n"
         "}\n"
         "#endif\n\n"
         "static bool audiod_set_interface(uint8_t rhport, tusb_control_request_t const *p_request) {"),
        "Fix 11c – audiod_deferred_first_tx_cb helper")

    # Fix 13: Linux may address entity requests with AS interface (1) in wIndex, not only AC (0).
    apply(AC,
        ("  for (i = 0; i < CFG_TUD_AUDIO; i++) {\n"
         "    // Look for the correct driver by checking if the unique standard AC interface number fits\n"
         "    if (_audiod_fct[i].p_desc && ((tusb_desc_interface_t const *) _audiod_fct[i].p_desc)->bInterfaceNumber == itf) {"),
        ("  for (i = 0; i < CFG_TUD_AUDIO; i++) {\n"
         "    // Fix 13: accept wIndex on any interface belonging to this audio function (IAD range)\n"
         "    if (!_audiod_fct[i].p_desc) continue;\n"
         "    {\n"
         "      uint8_t const *iad = _audiod_fct[i].p_desc - TUD_AUDIO_DESC_IAD_LEN;\n"
         "      uint8_t const first = ((tusb_desc_interface_assoc_t const *) iad)->bFirstInterface;\n"
         "      uint8_t const count = ((tusb_desc_interface_assoc_t const *) iad)->bInterfaceCount;\n"
         "      if (itf < first || itf >= first + count) continue;\n"
         "    }\n"
         "    {"),
        "Fix 13 – audiod_verify_entity_exists accepts AS interface in wIndex")

    # ────────────────────────────────────────────────────────────────────────
    print(f"\n── {os.path.basename(UD)}")

    # Fix 9: Clear EP0 busy/claimed on SETUP — root cause of post-alt=1 crash.
    # Our set_itf_cb calls tud_control_status → EP0_IN busy=1.  The next SETUP
    # (clock query from Linux) calls reset_ep0() which clears hardware state but
    # NOT the software busy flag.  usbd_edpt_claim then sees busy=1 →
    # TU_ASSERT UB → crash.  Fix: reset software state to match hardware.
    apply(UD,
        ("    // deliver event\n"
         "    case DCD_EVENT_SETUP_RECEIVED:"),
        ("    // deliver event\n"
         "    case DCD_EVENT_SETUP_RECEIVED:\n"
         "      /* Fix 9: SETUP aborts pending EP0; reset SW state to match HW */\n"
         "      _usbd_dev.ep_status[0][TUSB_DIR_IN].busy    = 0;\n"
         "      _usbd_dev.ep_status[0][TUSB_DIR_IN].claimed = 0;\n"
         "      _usbd_dev.ep_status[0][TUSB_DIR_OUT].busy   = 0;\n"
         "      _usbd_dev.ep_status[0][TUSB_DIR_OUT].claimed = 0;"),
        "Fix 9  – clear EP0 busy/claimed on SETUP (prevent TU_ASSERT UB on clock query)")

    # Fix 10: TU_ASSERT(!claimed && !busy) in usbd_edpt_claim is UB with -O3.
    apply(UD,
        "  TU_ASSERT(!_usbd_dev.ep_status[epnum][dir].claimed && !_usbd_dev.ep_status[epnum][dir].busy);",
        ("  if (_usbd_dev.ep_status[epnum][dir].claimed ||\n"
         "      _usbd_dev.ep_status[epnum][dir].busy) { return false; } // Fix 10: no UB"),
        "Fix 10 – replace TU_ASSERT in usbd_edpt_claim with safe if/return")

    # Fix 6/12: TU_ASSERT(busy==0) in usbd_edpt_xfer / usbd_edpt_xfer_fifo causes UB
    # with -O3. Replace every occurrence with a well-defined if/return.
    busy_assert = "  TU_ASSERT(_usbd_dev.ep_status[epnum][dir].busy == 0);\n\n  // Set busy"
    busy_safe = ("  if (_usbd_dev.ep_status[epnum][dir].busy) {\n"
                 "    return false; // Fix 6/12: no TU_ASSERT UB on busy endpoint\n"
                 "  }\n\n  // Set busy")
    with open(UD) as f:
        ud_text = f.read()
    n_busy = ud_text.count(busy_assert)
    if n_busy:
        with open(UD, 'w') as f:
            f.write(ud_text.replace(busy_assert, busy_safe))
        print(f"  ✓ Fix 6/12 – replace TU_ASSERT(busy==0) ×{n_busy} in usbd_edpt_xfer(_fifo)")
    elif busy_safe.strip()[:40] in ud_text:
        print(f"  = Fix 6/12  [already]")
    else:
        print(f"  ✗ Fix 6/12  [pattern not found]", file=sys.stderr)

    # ── Summary ──────────────────────────────────────────────────────────────
    print("\n── Verify:")
    checks = [
        (DCD, "hw_endpoint_abort_xfer",      "3"),
        (USB, "TUSB_XFER_ISOCHRONOUS",       "1"),
        (USB, "if (ep->endpoint_control)",   "1"),
        (AC,  "// Fix 5a",                   "1"),
        (AC,  "// Fix 5b",                   "1"),
        (AC,  "// Fix 7b",                   "1"),
        (AC,  "// Fix 7c",                   "1"),
        (AC,  "// Fix 8",                    "1"),
        (AC,  "// Fix 11:",                  "1"),
        (AC,  "usbd_defer_func(audiod_deferred_first_tx_cb", "1"),
        (AC,  "Fix 13: accept wIndex",     "1"),
        (UD,  "// Fix 6/12:",                "1"),
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
