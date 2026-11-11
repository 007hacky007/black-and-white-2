/*
 * d3d_register.c - the graphics layers' imports and method implementations
 * (plan 4), registered before win32hle's tables (register.c).
 */
#include "bw2.h"

void d3d9_adapter_register(void);
void ddraw7_register(void);
void d3d9_device_register(void);
void d3d9_state_register(void);
void d3d9_surface_register(void);
void d3d9_texture_register(void);
void d3d9_buffer_register(void);
void d3d9_copy_register(void);

void bw2_d3d_register(void) {
    d3d9_adapter_register();
    ddraw7_register();
    d3d9_device_register();
    d3d9_state_register();
    d3d9_surface_register();
    d3d9_texture_register();
    d3d9_buffer_register();
    d3d9_copy_register();
}
