/*
 * d3d_register.c - the graphics layers' imports and method implementations
 * (plan 4), registered before win32hle's tables (register.c).
 */
#include "bw2.h"

void d3d9_adapter_register(void);
void ddraw7_register(void);

void bw2_d3d_register(void) {
    d3d9_adapter_register();
    ddraw7_register();
}
