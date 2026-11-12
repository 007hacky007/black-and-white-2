/*
 * ui.h - D3DX's 2D layer (plan 7): ID3DXSprite (d3dx_sprite.c), and later
 * ID3DXFont. Both reach the device through its guest vtable (d3d_call), so
 * state blocks, Get calls and draws see what the game would see.
 */
#ifndef BW2_UI_H
#define BW2_UI_H
#include <stdint.h>

/* ID3DXSprite::Begin flags */
#define D3DXSPRITE_DONOTSAVESTATE          0x01u
#define D3DXSPRITE_DONOTMODIFY_RENDERSTATE 0x02u
#define D3DXSPRITE_OBJECTSPACE             0x04u
#define D3DXSPRITE_BILLBOARD               0x08u
#define D3DXSPRITE_ALPHABLEND              0x10u
#define D3DXSPRITE_SORT_TEXTURE            0x20u
#define D3DXSPRITE_SORT_DEPTH_FRONTTOBACK  0x40u
#define D3DXSPRITE_SORT_DEPTH_BACKTOFRONT  0x80u

/* A guest ID3DXSprite on device (reference count 1; it holds a device reference). */
uint32_t d3dx_sprite_create(uint32_t device);
/* How many DrawPrimitiveUP calls the sprite layer has made (tests). */
uint32_t d3dx_sprite_draw_calls(void);

#endif
