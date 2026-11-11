/*
 * bw2.h - the BW2 engine's own runtime layer on top of pcrecomp's win32hle.
 *
 * Exit codes (plan 3): 0 the guest exited normally (or its own code), 2 boot
 * failure, 3 tripwire (the user decides), 4 reached a layer a later plan owns.
 * Every other loud failure ends in abort() after naming what failed.
 */
#ifndef BW2_H
#define BW2_H
#include <stdint.h>
#include "win32hle.h"

#define BW2_EXIT_BOOT     2
#define BW2_EXIT_TRIPWIRE 3
#define BW2_EXIT_LATER    4

/* dispatch.c: an O(1) recomp_lookup over the generated dispatch table. */
void bw2_dispatch_init(void);
int  bw2_dispatch_selftest(void);       /* prints "dispatch ok <n>"; 0 on success */

/* main.c: register the shim modules, BW2's own first so they take precedence. */
void bw2_register_shims(void);

#endif
