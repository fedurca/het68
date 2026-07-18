// het68_time.h — wall-clock time synced over UART (required for DET timestamps).
#pragma once
#include <stdint.h>
#include <stdbool.h>

void het68_time_init(void);

// TIME SYNC <unix_epoch_seconds>
bool het68_time_sync(uint32_t unix_epoch_sec);

bool het68_time_synced(void);
uint32_t het68_time_epoch_sec(void);   // 0 if not synced
uint32_t het68_time_epoch_ms(void);    // 0 if not synced
uint32_t het68_time_synced_at(void);   // epoch when SYNC was applied (0 if never)

void het68_time_dump_uart(void);
