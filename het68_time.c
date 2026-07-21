// het68_time.c — epoch clock anchored by UART TIME SYNC or RID System time.
#include "het68_time.h"
#include "debug_io.h"
#include "remote_id.h"
#include "pico/stdlib.h"

static bool g_synced;
static uint32_t g_epoch0;           // unix seconds at sync instant
static absolute_time_t g_at0;       // pico time at sync instant
static uint32_t g_synced_at_epoch;  // copy of g_epoch0 for status
static het68_time_src_t g_source;
static uint8_t g_quality_base;

void het68_time_init(void) {
    g_synced = false;
    g_epoch0 = 0;
    g_synced_at_epoch = 0;
    g_source = HET68_TIME_SRC_NONE;
    g_quality_base = 0;
    g_at0 = get_absolute_time();
}

bool het68_time_sync_from(uint32_t unix_epoch_sec, het68_time_src_t src,
                          uint8_t quality) {
    if (unix_epoch_sec < 1700000000u) return false; // reject nonsense / year < 2023
    if (src == HET68_TIME_SRC_NONE) return false;
    if (quality > 100u) quality = 100u;
    g_epoch0 = unix_epoch_sec;
    g_synced_at_epoch = unix_epoch_sec;
    g_at0 = get_absolute_time();
    g_synced = true;
    g_source = src;
    g_quality_base = quality;
    return true;
}

bool het68_time_sync(uint32_t unix_epoch_sec) {
    return het68_time_sync_from(unix_epoch_sec, HET68_TIME_SRC_UART, 100u);
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

het68_time_src_t het68_time_source(void) {
    return g_synced ? g_source : HET68_TIME_SRC_NONE;
}

const char *het68_time_source_name(het68_time_src_t src) {
    switch (src) {
        case HET68_TIME_SRC_UART:     return "uart";
        case HET68_TIME_SRC_RID:      return "rid";
        case HET68_TIME_SRC_ACOUSTIC: return "acoustic";
        default:                      return "none";
    }
}

uint32_t het68_time_age_sec(void) {
    if (!g_synced) return 0;
    int64_t us = absolute_time_diff_us(g_at0, get_absolute_time());
    if (us < 0) us = 0;
    return (uint32_t)(us / 1000000);
}

uint8_t het68_time_quality_base(void) {
    return g_synced ? g_quality_base : 0;
}

uint8_t het68_time_quality(void) {
    if (!g_synced) return 0;
    uint32_t age = het68_time_age_sec();
    uint32_t decay = age / 60u; // ~1 point per minute
    if (decay >= (uint32_t)g_quality_base) return 1u;
    return (uint8_t)(g_quality_base - (uint8_t)decay);
}

void het68_time_dump_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("TIME synced=");
    dbg_puts(g_synced ? "1" : "0");
    dbg_puts(" source=");
    dbg_puts(het68_time_source_name(het68_time_source()));
    dbg_puts(" epoch=");
    dbg_putu32(het68_time_epoch_sec());
    dbg_puts(" synced_at=");
    dbg_putu32(g_synced_at_epoch);
    dbg_puts(" age_s=");
    dbg_putu32(het68_time_age_sec());
    dbg_puts(" quality=");
    dbg_putu32(het68_time_quality());
    if (remote_id_available()) {
        dbg_puts(" rid_unix=");
        dbg_putu32(remote_id_last_unix());
        dbg_puts(" rid_syncs=");
        dbg_putu32(remote_id_time_sync_count());
    }
    dbg_putc('\n');
    dbg_line_unlock(lock);
}

void het68_time_info_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("TIME INFO synced=");
    dbg_puts(g_synced ? "1" : "0");
    dbg_puts(" source=");
    dbg_puts(het68_time_source_name(het68_time_source()));
    dbg_puts(" age_s=");
    dbg_putu32(het68_time_age_sec());
    dbg_puts(" quality=");
    dbg_putu32(het68_time_quality());
    dbg_puts(" quality_base=");
    dbg_putu32(het68_time_quality_base());
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
