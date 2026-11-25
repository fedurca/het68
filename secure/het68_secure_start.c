// het68_secure_start.c — Secure bring-up + H68P gate + Core1 detection start.
#include "het68_secure.h"
#include "het68_flash_map.h"
#include "h68p.h"
#include "h68p_pubkey.h"
#include "doa.h"
#include "debug_io.h"
#include "core1_launch.h"

#include "pico/unique_id.h"
#include "hardware/sync.h"

#include <string.h>

void het68_sau_configure(void);
void het68_plugin_status_set(const het68_plugin_status_t *s);
bool het68_plugin_pick_best(const uint8_t serial[H68P_SERIAL_LEN],
                            h68p_view_t *out, uint32_t *slot_out);

#ifndef HET68_PLUGIN_REQUIRED
#define HET68_PLUGIN_REQUIRED 0
#endif

static void fill_serial(uint8_t out[H68P_SERIAL_LEN]) {
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    memcpy(out, uid.id, H68P_SERIAL_LEN);
}

void het68_secure_init(void) {
    het68_sau_configure();
}

#if HET68_PLUGIN_EXTERNAL
// Verify already done by pick_best. Copy code into Secure SRAM (no XIP exec),
// zero BSS tail, then launch Core1 at Thumb entry.
static bool start_external_plugin(const h68p_view_t *v) {
    uint32_t code = v->hdr->code_size;
    uint32_t bss = v->hdr->bss_size;
    if (code == 0u || code > HET68_PLUGIN_SRAM_BYTES) return false;
    if (bss > HET68_PLUGIN_SRAM_BYTES - code) return false;
    uint8_t *dst = (uint8_t *)(uintptr_t)HET68_PLUGIN_SRAM_BASE;
    memcpy(dst, v->payload, code);
    if (bss) memset(dst + code, 0, bss);
    __dmb();
    __asm volatile ("dsb");
    __asm volatile ("isb");
    uintptr_t entry_addr = (uintptr_t)(HET68_PLUGIN_SRAM_BASE + v->hdr->entry_off);
    // Thumb bit required for Cortex-M function pointers.
    void (*entry)(void) = (void (*)(void))(entry_addr | 1u);
    if (!het68_launch_core1(entry)) return false;
    return true;
}
#endif

bool het68_secure_start(void) {
    het68_secure_init();

    het68_plugin_status_t st;
    memset(&st, 0, sizeof st);
    fill_serial(st.serial);

    h68p_view_t view;
    uint32_t slot = 0;
    bool have = het68_plugin_pick_best(st.serial, &view, &slot);
    if (have) {
        st.source = HET68_PLUGIN_FLASH;
        st.last_status = H68P_OK;
        st.slot = slot;
        st.code_size = view.hdr->code_size;
        st.rollback = view.hdr->rollback;
        dbg_puts("DET PLUGIN: verified flash slot=");
        dbg_putu32(slot);
        dbg_puts(" rb=");
        dbg_putu32(view.hdr->rollback);
        dbg_puts(" code=");
        dbg_putu32(view.hdr->code_size);
        dbg_putc('\n');

#if HET68_PLUGIN_EXTERNAL
        if (!start_external_plugin(&view)) {
            st.source = HET68_PLUGIN_INVALID;
            st.last_status = H68P_ERR_SIZE;
            het68_plugin_status_set(&st);
            dbg_puts("DET PLUGIN: external load FAILED\n");
            return false;
        }
        st.core1_running = true;
        het68_plugin_status_set(&st);
        return true;
#else
        // Lab / transitional: signed H68P is a device gate; detection still builtin.
        doa_start();
        st.core1_running = (g_core1_alive != 0u);
        het68_plugin_status_set(&st);
        return st.core1_running;
#endif
    }

    // No valid device-bound plugin in flash.
    st.last_status = H68P_ERR_MAGIC;
#if HET68_PLUGIN_REQUIRED
    st.source = HET68_PLUGIN_NONE;
    st.core1_running = false;
    het68_plugin_status_set(&st);
    dbg_puts("DET PLUGIN: required — none valid for this serial\n");
    return false;
#else
    st.source = HET68_PLUGIN_BUILTIN;
    dbg_puts("DET PLUGIN: none in flash — starting builtin detection\n");
    doa_start();
    st.core1_running = (g_core1_alive != 0u);
    het68_plugin_status_set(&st);
    return st.core1_running;
#endif
}
