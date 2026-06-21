# Upstream delta — které patche existují jen u nás

Analýza našich TinyUSB / pico-sdk patchů (viz [`PATCHES.md`](PATCHES.md) a
[`patches/apply_all.py`](patches/apply_all.py)) proti **nejnovějšímu upstreamu**.

## Verze

| Knihovna | Naše | Nejnovější | Pozn. |
|---|---|---|---|
| pico-sdk | 2.2.0 | **2.2.0** | aktuální (žádný novější release) |
| TinyUSB (uvnitř pico-sdk) | 0.18.0 | **0.20.0** (master 0.20.1-dev) | jádro divergence |

> ⚠️ TinyUSB 0.20 je rozsáhle přerefaktorovaný (nové signatury
> `usbd_edpt_xfer(..., bool is_isr)`, `CFG_TUD_EDPT_DEDICATED_HWFIFO`,
> `dpram_buf` / `EPSTATE_ACTIVE`, bitmask `ep_status`, nové `tud_audio_tx_done_isr`).
> Naše patche jsou psané proti 0.18 a na 0.20 by se musely celé přeportovat.
> Porovnání bylo provedeno proti staženému `hathach/tinyusb@master`.

---

## Co už upstream vyřešil (po upgradu na 0.20 by byl náš patch nadbytečný)

| Fix | Stav v upstreamu 0.20 |
|---|---|
| **Fix 1c** – `dcd_edpt_iso_activate` abort před re-enable | ✅ `if (ep->state == EPSTATE_ACTIVE) hw_endpoint_abort_xfer(ep);` |
| **Fix 1b** – `reset_ep0` používá `hw_endpoint_abort_xfer` | ✅ strukturálně přítomno |
| **Fix 2** – reset transferu před `dcd_event_xfer_complete` | ✅ `xferred_len` uložen, pak reset, pak notify |
| **Fix 4** – NULL-guard `endpoint_control` na RP2350 | ✅ přepsáno na `get_ep_ctrl()` + `if (ep_reg != NULL)` |
| **Fix 3** – ISO ZLP `panic` na neaktivním EP | ✅ refaktorováno (`panic` v té podobě pryč; cesta přepsána na `rp2usb_xfer_continue`) |
| **Fix 10** – `usbd_edpt_claim` přes `TU_ASSERT` | ✅ `return tu_edpt_claim(...)` |

---

## Co existuje stále JEN U NÁS (upstream 0.20 to nemá)

| Fix | Proč je pořád lokální | Závažnost |
|---|---|---|
| **Fix 1a** – vynechání HW `while(abort_done)` spinu | Upstream spin **stále má**, jen pod `rp2040_chip_version() >= 2`. Na RP2350 nedoběhne → zámrz USB ISR. | 🔴 kritické pro RP2350 |
| **Fix 11/11c** – odložení prvního ISO IN přes `usbd_defer_func` | Upstream startuje první přenos **inline jako ZLP** (`usbd_edpt_xfer(..., 0, false)`) před `tud_control_status`; žádné `usbd_defer_func`. | 🔴 klíčové pro enumeraci |
| **Fix 13** – `verify_entity_exists` přijme `wIndex` na AS rozhraní / IAD rozsah | Upstream `audiod_verify_entity_exists` matchne **jen AC** `bInterfaceNumber == itf`. | 🟠 vysoká (Linux host) |
| **Fix 9** – vyčištění EP0 busy/claimed na `SETUP` | Upstream na `DCD_EVENT_SETUP_RECEIVED` jen `_usbd_queued_setup++`; EP0 stav nečistí. | 🟠 vysoká |
| **Fix 6/12** – `TU_ASSERT(busy==0)` → bezpečné `if/return` | Upstream **stále má** `TU_ASSERT((... & TU_EDPT_STATE_BUSY) == 0)` na obou místech. | 🟠 (UB s `-O3`) |
| **Fix 5a** – `usbd_edpt_release` po `clear_stall` | Upstream tam má jen `clear_stall` + komentář `THIS IS A WORKAROUND!`. | 🟡 střední |
| **Fix 5b/5c/5d, 7b/7c, 8** – odstranění `TU_VERIFY(...)` kolem callback/xfer | Upstream `TU_VERIFY` používá dál; naše odstranění je specifické pro `-O3`. | 🟡 (build/`-O3`) |

---

## Závěr

- **6 fixů už je v upstreamu** (1b, 1c, 2, 3, 4, 10) — po upgradu na 0.20 by šly zahodit.
- **7 skupin fixů zůstává unikátních u nás** (1a, 5a, 5b–8, 6/12, 9, 11/11c, 13).
  RP2350-kritické: **Fix 1a** (abort spin) a **Fix 11/11c** (start streamingu);
  pro Linux host: **Fix 13** + **Fix 9**.
- Skupina „odstranění `TU_VERIFY`/`TU_ASSERT`" (5b–8, 6/12) jde proti filozofii
  upstreamu — je to obrana proti `-O3` UB v našem konkrétním buildu.

**Doporučení:** Upgrade TinyUSB na 0.20 smaže ~6 patchů, ale **nevyřeší** RP2350
abort spin, deferred start, AS/IAD entity lookup ani `-O3 TU_ASSERT` UB — ty by se
musely přeportovat na nové API. Protože pico-sdk 2.2.0 oficiálně veze 0.18.0,
upgrade = ruční bump submodulu + přepis patchů. Stav je teď prokazatelně funkční,
takže dává smysl zůstat na 0.18 a unikátní fixy si držet (a nabídnout je upstreamu —
hlavně **1a**, **11**, **13** jsou obecně užitečné pro RP2350).

Upstream-worthy kandidáti pro PR: **Fix 1a** (RP2350 abort spin) a **Fix 13**
(entity lookup přes AS rozhraní) — oba jsou samostatné a host/silicon-relevantní.
