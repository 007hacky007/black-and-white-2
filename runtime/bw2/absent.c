/*
 * absent.c - what the win32hle files the engine does not link would have
 * defined (runtime/bw2 links win32hle's headless core: no SDL2, no ffmpeg, no
 * DirectX 6; see bw2/build.py WIN32HLE_CORE). Shim tables are empty, so the
 * imports they held stay unanswered and stop the run by name when called;
 * functions abort, naming themselves.
 */
#include <stdio.h>
#include <stdlib.h>
#include "win32hle.h"

#define ABSENT(name) fprintf(stderr, "[bw2] %s: not linked in this engine\n", name), abort()

const win32hle_shim win32hle_gdidc[] = { { NULL, NULL } };
const win32hle_shim win32hle_dsound[] = { { NULL, NULL } };
const win32hle_shim win32hle_ddraw[] = { { NULL, NULL } };
const win32hle_shim win32hle_bink[] = { { NULL, NULL } };
const win32hle_shim win32hle_mss[] = { { NULL, NULL } };
void hle_dsound_register_com(void) { ABSENT("hle_dsound_register_com"); }
uint32_t hle_gdi_make_dib(int w, int h, int bpp, uint8_t *bits, int topdown, const uint32_t *ctab,
                          int ncolors, int is555) {
    (void)w; (void)h; (void)bpp; (void)bits; (void)topdown; (void)ctab; (void)ncolors; (void)is555;
    ABSENT("hle_gdi_make_dib (gdidc.c)");
}
void hle_gdi_release_dc(uint32_t hdc) { (void)hdc; ABSENT("hle_gdi_release_dc (gdidc.c)"); }
uint32_t hle_gdi_window_dc(void) { ABSENT("hle_gdi_window_dc (gdidc.c)"); }
