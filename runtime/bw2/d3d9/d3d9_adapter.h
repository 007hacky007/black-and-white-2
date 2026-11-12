/*
 * d3d9_adapter.h - the one adapter this device shows the guest (plan 4
 * Global Constraints): its identity, display, modes and caps.
 */
#ifndef D3D9_ADAPTER_H
#define D3D9_ADAPTER_H
#include <stdint.h>

#define BW2_GPU_DRIVER      "bw2metal.dll"
#define BW2_GPU_DESCRIPTION "BW2 virtual GPU (Metal)"
#define BW2_GPU_VENDOR      0x106Bu
#define BW2_GPU_DEVICE      0x0001u
#define BW2_GPU_VERSION_HI  ((6u << 16) | 14u)      /* 6.14 */
#define BW2_GPU_VERSION_LO  ((10u << 16) | 0u)      /* .10.0 */
#define BW2_VIDMEM          (512u << 20)
extern const uint8_t bw2_gpu_guid[16];

void d3d_display_size(uint32_t *w, uint32_t *h);   /* the main display in pixels; 1920x1080 headless */
/* The mode the game sees (GetAdapterDisplayMode, IDirect3DDevice9::GetDisplayMode, GetSystemMetrics):
 * the desktop's, or the full-screen device's back buffer while one lives, as a Windows mode change
 * gives. The mode list and GetRasterStatus stay on d3d_display_size. */
void d3d_current_mode(uint32_t *w, uint32_t *h);
/* A device was created or Reset (fullscreen and its back buffer), or went (gone): updates the current
 * mode and win32hle's screen size. */
void d3d_mode_device(const void *dev, int fullscreen, uint32_t w, uint32_t h, int gone);
uint32_t d3d_mode_count(uint32_t fmt);
int d3d_mode(uint32_t fmt, uint32_t i, uint32_t *w, uint32_t *h);   /* 0 past the end */
void d3d_fill_caps(uint32_t caps_va);              /* a D3DCAPS9 (304 bytes) */

#endif
