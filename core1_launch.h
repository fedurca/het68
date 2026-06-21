// core1_launch.h — safe core1 bring-up after Debug Probe / OpenOCD flash.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef bool (*het68_core1_verify_fn)(void);

bool het68_launch_core1_verify(void (*entry)(void), het68_core1_verify_fn verify);
bool het68_launch_core1(void (*entry)(void));

void het68_core1_setup(void);
void het68_core1_settle_scan(void);

extern volatile uint32_t g_core1_alive;
extern volatile uint32_t g_core1_hb;
