# TinyUSB / pico-sdk patches (mimo repo)

Přehled všech zásahů do **vendrované `pico-sdk` / `TinyUSB`** (tj. mimo zdrojáky
tohoto repa), které byly nutné pro zprovoznění RP2350 UAC2 6kanálové zvukové karty.

Všechny patche aplikuje idempotentně skript [`patches/apply_all.py`](patches/apply_all.py):
nejdřív vrátí dotčené soubory na `git HEAD`, pak je znovu zapatchuje. `./build.sh`
ho volá automaticky a na konci vypíše `✓ All patches OK`.

## Dotčené soubory v SDK

| Soubor | Role |
|---|---|
| `pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c` | RP2040/RP2350 device controller driver (DCD) |
| `pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c` | nízkoúrovňová HW vrstva USB |
| `pico-sdk/lib/tinyusb/src/class/audio/audio_device.c` | UAC2 class driver |
| `pico-sdk/lib/tinyusb/src/device/usbd.c` | jádro USB device stacku |
| `pico-sdk/lib/tinyusb/src/device/usbd_control.c` | EP0 control (jen debug checkpointy, v produkci vypnuté) |

## Tři opakující se kořenové příčiny

1. **RP2350 ≠ RP2040 v DCD** — hardwarový `EP_ABORT` spin na RP2350 nikdy nedoběhne
   (zamrzne USB ISR) a `endpoint_control` pro EP0 je `NULL`.
2. **`TU_VERIFY` / `TU_ASSERT` + `-O3` = undefined behavior** — makra expandují na
   `return;` uvnitř `bool` funkce; GCC s `-O3` může „propadnout" (fall-through)
   místo návratu → pády / zámrzy.
3. **Časování EP0 vs. start ISO streamingu** — start prvního ISO IN uvnitř
   `SET_INTERFACE` závodil se status fází EP0, takže Linux padal na timeoutu
   clock-validity (`err -110`).

---

## 1) `dcd_rp2040.c` — DCD (RP2350 specifika)

### Fix 1a — `hw_endpoint_abort_xfer()` bez HW abort spinu
Zruší probíhající transfer **bez** čekání na `abort_done`. Na RP2350 ten HW bit
nikdy nesepne → původní `while(...abort_done...)` zamrzne USB ISR.

```c
// Abort a pending transfer. EP_ABORT hardware spin (abort_done) is skipped:
// on RP2350 it never completes, freezing the USB ISR.
static void hw_endpoint_abort_xfer(struct hw_endpoint* ep) {
  uint32_t buf_ctrl = USB_BUF_CTRL_SEL;
  if (ep->next_pid) buf_ctrl |= USB_BUF_CTRL_DATA1_PID;
  _hw_endpoint_buffer_control_set_value32(ep, buf_ctrl);
  hw_endpoint_reset_transfer(ep);
}
```

### Fix 1b — použít `abort_xfer` v `reset_ep0()`
Nahradí původní blok s `usb_hw_set->abort` + `while(abort_done)` voláním nové
bezpečné funkce.

```c
ep->next_pid = 1u;
if (ep->active) {
  hw_endpoint_abort_xfer(ep);   // místo HW abort spinu
}
```

### Fix 1c — `dcd_edpt_iso_activate()`: abort před re-enable
Při (re)aktivaci izochronního endpointu nejdřív zruší případný viset zůstavší
transfer, jinak se ISO IN po přepnutí alt mohl rozbít.

```c
struct hw_endpoint* ep = hw_endpoint_get_by_addr(ep_desc->bEndpointAddress);
TU_ASSERT(ep->hw_data_buf != NULL);
if (ep->active) { hw_endpoint_abort_xfer(ep); }   // nové
ep->wMaxPacketSize = ep_desc->wMaxPacketSize;
hw_endpoint_enable(ep);
```

### Fix 2 — reset stavu EP před notifikací stacku (upstream `4bfba6b`)
Nejdřív vyresetovat transfer, až pak poslat `dcd_event_xfer_complete`. Jinak
handler události viděl ještě „starý" stav endpointu.

```c
if (done) {
  uint16_t const xferred_len = ep->xferred_len;
  hw_endpoint_reset_transfer(ep);                  // reset PŘED
  dcd_event_xfer_complete(0, ep->ep_addr, xferred_len, XFER_RESULT_SUCCESS, true);
```

---

## 2) `rp2040_usb.c` — HW vrstva (RP2350 + ISO)

### Fix 3 — ISO ZLP: `return false` místo `panic`
Při pokračování transferu na neaktivním ISO endpointu (typicky zero-length packet)
původní kód `panic`oval. Pro ISO je to legitimní stav → tiše vrátit `false`.

```c
if (!ep->active) {
  if (ep->transfer_type == TUSB_XFER_ISOCHRONOUS) {
    hw_endpoint_lock_update(ep, -1);
    return false;
  }
  panic("Can't continue xfer on inactive ep %02X", ep->ep_addr);
}
```

### Fix 4 — NULL-guard `ep->endpoint_control` (RP2350)
Na RP2350 má EP0 `endpoint_control == NULL`; bezpodmínečný zápis crashnul.

```c
if (ep->endpoint_control) *ep->endpoint_control = ep_ctrl;  // EP0=NULL on RP2350
```

---

## 3) `audio_device.c` — UAC2 class driver

### Fix 5a — uvolnit „busy" flag po ISO abortu
`usbd_edpt_clear_stall` čistí busy jen při stallu; přidané `usbd_edpt_release` ho
vyčistí vždy, aby `audiod_tx_done_cb` mohl nastartovat nový transfer.

```c
usbd_edpt_clear_stall(rhport, ep_addr);
usbd_edpt_release(rhport, ep_addr);   // Fix 5a: clear busy after ISO abort
```

### Fix 5b/5c/5d, 7b/7c, 8 — odstranění `TU_VERIFY(...)` kolem callback/xfer volání
Všude, kde se volalo `TU_VERIFY(...)` kolem callbacku nebo xferu, je makro
nahrazeno **prostým voláním**. `TU_VERIFY` = `if(!(x)) return;` v `bool` funkci →
s `-O3` UB.

```c
audiod_tx_done_cb(rhport, &_audiod_fct[func_id]);                 // Fix 5b
tud_audio_tx_done_pre_load_cb(rhport, idx, audio->ep_in, alt);    // Fix 5c
tud_audio_tx_done_post_load_cb(rhport, n, idx, audio->ep_in,alt); // Fix 5d
usbd_edpt_xfer(rhport, audio->ep_in, audio->lin_buf_in, n_bytes_tx);          // Fix 7b
usbd_edpt_xfer_fifo(rhport, audio->ep_in, &audio->ep_in_ff, n_bytes_tx);      // Fix 7c
audiod_tx_done_cb(rhport, audio);                                 // Fix 8 (v audiod_xfer_complete)
```

### Fix 11 + 11c — odložit první ISO IN za status fázi `SET_INTERFACE` (klíčové pro enumeraci)
Původně se první `audiod_tx_done_cb` (= naplánování prvního IN) volal **uvnitř**
`audiod_set_interface`, ještě před `tud_control_status`. To závodilo se status fází
EP0 a Linux pak timeoutoval na `GET_CUR CLK_VALID` (`err -110`). Fix 11 to volání
odstraní; Fix 11c ho přesune **za** `tud_control_status` přes `usbd_defer_func`
(proběhne v dalším tiku USB smyčky).

```c
tud_control_status(rhport, p_request);

// Fix 11c: defer first ISO IN to next USB event loop tick
#if CFG_TUD_AUDIO_ENABLE_EP_IN
if (alt != 0 && _audiod_fct[func_id].ep_in != 0 &&
    _audiod_fct[func_id].ep_in_as_intf_num == itf) {
  usbd_defer_func(audiod_deferred_first_tx_cb, (void*)(uintptr_t)func_id, false);
}
#endif
return true;
```

Pomocný callback (obsahuje i neblokující UART checkpoint `DEFER`, viditelný v logu):

```c
static void audiod_deferred_first_tx_cb(void* param) {
  dbg_puts("DEFER\n");                       // het68 UART checkpoint
  uint8_t func_id = (uint8_t)(uintptr_t)param;
  audiod_tx_done_cb(_audiod_fct[func_id].rhport, &_audiod_fct[func_id]);
}
```

### Fix 13 — `audiod_verify_entity_exists` přijme `wIndex` na AC i AS rozhraní
Linux občas adresuje class požadavky přes **AS** rozhraní (1), ne jen přes **AC**
(0). Patch je **aditivní**: matchne původní AC rozhraní, nebo jakékoli rozhraní
v rozsahu IAD dané audio funkce — takže špatný IAD průchod nikdy nezahodí
požadavek, který původní kód přijal.

```c
uint8_t const ac_itf = ((tusb_desc_interface_t const*)_audiod_fct[i].p_desc)->bInterfaceNumber;
bool match = (ac_itf == itf);
if (!match) {
  uint8_t const *iad   = _audiod_fct[i].p_desc - TUD_AUDIO_DESC_IAD_LEN;
  uint8_t const first  = ((tusb_desc_interface_assoc_t const*)iad)->bFirstInterface;
  uint8_t const count  = ((tusb_desc_interface_assoc_t const*)iad)->bInterfaceCount;
  if (itf >= first && itf < (uint8_t)(first + count)) match = true;
}
if (!match) continue;
```

---

## 4) `usbd.c` — jádro device stacku

### Fix 9 — vyčistit EP0 busy/claimed při příchodu `SETUP` (kořen pádu po alt=1)
`set_itf_cb` volá `tud_control_status` → EP0_IN `busy=1`. Další `SETUP` (clock query
z Linuxu) projde `reset_ep0()`, který srovná HW, ale **ne** SW busy flag.
`usbd_edpt_claim` pak vidí `busy=1` → `TU_ASSERT` UB → pád. Fix srovná SW stav s HW
hned na `DCD_EVENT_SETUP_RECEIVED`.

```c
case DCD_EVENT_SETUP_RECEIVED:
  /* Fix 9: SETUP aborts pending EP0; reset SW state to match HW */
  _usbd_dev.ep_status[0][TUSB_DIR_IN].busy     = 0;
  _usbd_dev.ep_status[0][TUSB_DIR_IN].claimed  = 0;
  _usbd_dev.ep_status[0][TUSB_DIR_OUT].busy    = 0;
  _usbd_dev.ep_status[0][TUSB_DIR_OUT].claimed = 0;
```

> Pozn.: novější upstream tohle řeší jinak — skript to detekuje a hlásí
> „satisfied upstream", pokud už je to ve stromě.

### Fix 10 — `usbd_edpt_claim`: `TU_ASSERT` → bezpečné `if/return`
`TU_ASSERT(!claimed && !busy)` je s `-O3` UB. Nahrazeno definovaným chováním.
Pokud upstream už používá `tu_edpt_claim()`, skript hlásí „satisfied upstream".

```c
if (_usbd_dev.ep_status[epnum][dir].claimed ||
    _usbd_dev.ep_status[epnum][dir].busy) { return false; }  // Fix 10: no UB
```

### Fix 6/12 — `usbd_edpt_xfer` / `usbd_edpt_xfer_fifo`: `TU_ASSERT(busy==0)` → `if/return` (×2)
Stejná `-O3` UB příčina; aplikováno na obě místa, kde se kontroluje busy flag před
zahájením transferu.

```c
if (_usbd_dev.ep_status[epnum][dir].busy) {
  return false;   // Fix 6/12: no TU_ASSERT UB on busy endpoint
}
```

---

## 5) Instrumentace (mimo produkční tok)

- Do patchovaných souborů se vkládá `extern void dbg_puts(...)` + `DBG_CP()`, aby
  šly použít **neblokující UART checkpointy** (např. `DEFER`). V produkčním buildu
  jsou checkpointy v `rp2040_usb.c` / `usbd_control.c` vypnuté; aktivní zůstal jen
  `DEFER` v `audio_device.c`, který je užitečný a neblokující.
- Existuje i odkazový patch `patches/tinyusb-0.18.0-pr2937-iso-activate.patch`
  (TinyUSB PR 2937, ISO activate) — koncepčně odpovídá Fix 1c.

---

## Souhrn „proč to jede"

- **Enumerace netimeoutuje** (`err -110`): Fix 11/11c (odložený první ISO IN) +
  Fix 9 (čistý EP0 stav na SETUP).
- **Stack nepadá / nezamrzá**: Fix 1a/1b/1c (RP2350 abort), Fix 4 (NULL EP0 ctrl)
  a série odstranění `TU_VERIFY` / `TU_ASSERT` UB (5b–8, 6/10/12).
- **Streaming se rozjede a drží**: Fix 3 (ISO ZLP), Fix 5a (busy flag),
  Fix 13 (AS/AC wIndex) a na straně tohoto repa deterministické plnění FIFO
  v `tud_audio_tx_done_pre_load_cb` (viz `main.c`).
