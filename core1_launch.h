// core1_launch.h — safe core1 bring-up after Debug Probe / OpenOCD flash.
//
// OpenOCD "program ... verify reset exit" (see upload.sh) does not always leave
// core1 in a state where multicore_launch_core1() can succeed. Without an
// explicit PSM reset the secondary core may run briefly (~300 us) and then
// stop, which looks like a firmware hang. Always call het68_launch_core1()
// instead of multicore_launch_core1() directly.
#pragma once

void het68_launch_core1(void (*entry)(void));

// Call at the start of the core1 entry function (enables CP10/CP11 FPU).
void het68_core1_setup(void);
