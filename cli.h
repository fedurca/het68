// cli.h — simple UART CLI (help, time, status, DET, ENT, RID, drones).
#pragma once
#include <stdbool.h>

void cli_init(void);
void cli_print_help(void);

// Aggregated dump: time source/quality, DET/ENT/drone flash stats + contents.
void cli_print_status(void);

// Feed one received byte (non-blocking). Handles line assembly + commands.
void cli_rx_byte(int ch);

// Call once after USB mount / first attach to show help.
void cli_on_connect(void);
