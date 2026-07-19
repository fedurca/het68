// het68_time.c — epoch clock anchored by UART TIME SYNC (or RID System time).
#include "het68_time.h"
#include "debug_io.h"
#include "remote_id.h"
#include "pico/stdlib.h"

static bool g_synced;
static uint32_t g_epoch0;           // unix seconds at sync instant
static absolute_time_t g_at0;       // pico time at sync instant
static uint32_t g_synced_at_epoch;  // copy of g_epoch0 for status

void het68_time_init(void) {
    g_synced = false;
    g_epoch0 = 0;
    g_synced_at_epoch = 0;
    g_at0 = get_absolute_time();
}

bool het68_time_sync(uint32_t unix_epoch_sec) {
    if (unix_epoch_sec < 1700000000u) return false; // reject nonsense / year < 2023
    g_epoch0 = unix_epoch_sec;
    g_synced_at_epoch = unix_epoch_sec;
    g_at0 = get_absolute_time();
    g_synced = true;
    return true;
}

bool het68_time_synced(void) { return g_synced; }

uint32_t het68_time_epoch_sec(void) {
    if (!g_synced) return 0;
    int64_t us = absolute_time_diff_us(g_at0, get_absolute_time());
    if (us < 0) us = 0;
    return g_epoch0 + (uint32_t)(us / 1000000);
}

uint32_t het68_time_epoch_ms(void) {
    if (!g_synced) return 0;
    int64_t us = absolute_time_diff_us(g_at0, get_absolute_time());
    if (us < 0) us = 0;
    return g_epoch0 * 1000u + (uint32_t)(us / 1000);
}

uint32_t het68_time_synced_at(void) { return g_synced_at_epoch; }

void het68_time_dump_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("TIME synced=");
    dbg_puts(g_synced ? "1" : "0");
    dbg_puts(" epoch=");
    dbg_putu32(het68_time_epoch_sec());
    dbg_puts(" synced_at=");
    dbg_putu32(g_synced_at_epoch);
    if (remote_id_available()) {
        dbg_puts(" rid_unix=");
        dbg_putu32(remote_id_last_unix());
        dbg_puts(" rid_syncs=");
        dbg_putu32(remote_id_time_sync_count());
    }
    dbg_putc('\n');
    dbg_line_unlock(lock);
}
