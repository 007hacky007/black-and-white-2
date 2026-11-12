/*
 * d3d_register.c - the graphics layers' imports and method implementations
 * (plan 4; plan 5 adds shaders, the shader assembler and effects; plan 7 the
 * sprite, the font and the gdi32 memory-font exports; plan 9 the lifted D3DX's
 * forwarders and d3d9.dll's shader validator), registered before win32hle's
 * tables (register.c).
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
void d3d9_draw_register(void);
void d3d9_shader_register(void);
void d3d9_stateblock_register(void);
void d3d9_query_register(void);
void d3dx_math_register(void);
void d3dx_tex_register(void);
void d3dx_asm_register(void);
void fx_effect_register(void);
void d3dx_sprite_register(void);
void d3dx_font_register(void);
void bw2_font_register(void);
void d3dx_lifted_register(void);

void bw2_d3d_register(void) {
    d3d9_adapter_register();
    ddraw7_register();
    d3d9_device_register();
    d3d9_state_register();
    d3d9_surface_register();
    d3d9_texture_register();
    d3d9_buffer_register();
    d3d9_copy_register();
    d3d9_draw_register();
    d3d9_shader_register();
    d3d9_stateblock_register();
    d3d9_query_register();
    d3dx_math_register();
    d3dx_tex_register();
    d3dx_asm_register();
    fx_effect_register();
    d3dx_sprite_register();
    d3dx_font_register();
    bw2_font_register();
    d3dx_lifted_register();
}
