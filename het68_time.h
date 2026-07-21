// het68_time.h — wall-clock time synced over UART or OpenDroneID RID.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HET68_TIME_SRC_NONE     = 0,
    HET68_TIME_SRC_UART     = 1,
    HET68_TIME_SRC_RID      = 2,
    HET68_TIME_SRC_ACOUSTIC = 3,   // adopted from a synced peer over the piezo link
} het68_time_src_t;

void het68_time_init(void);

// TIME SYNC <unix_epoch_seconds> — treated as UART source, quality 100.
bool het68_time_sync(uint32_t unix_epoch_sec);

// Sync with explicit source + base quality (0..100).
bool het68_time_sync_from(uint32_t unix_epoch_sec, het68_time_src_t src,
                          uint8_t quality);

bool het68_time_synced(void);
uint32_t het68_time_epoch_sec(void);   // 0 if not synced
uint32_t het68_time_epoch_ms(void);    // 0 if not synced
uint32_t het68_time_synced_at(void);   // epoch when SYNC was applied (0 if never)

het68_time_src_t het68_time_source(void);
const char *het68_time_source_name(het68_time_src_t src);

// Seconds since last successful sync (0 if never synced).
uint32_t het68_time_age_sec(void);

// Effective quality 0..100: base quality decayed by age (~1 point / 60 s).
uint8_t het68_time_quality(void);

// Base quality from the last sync (before age decay).
uint8_t het68_time_quality_base(void);

void het68_time_dump_uart(void);       // compact TIME line
void het68_time_info_uart(void);       // TIME INFO: source / age / quality
