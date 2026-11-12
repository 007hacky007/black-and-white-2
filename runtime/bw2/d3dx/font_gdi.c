/*
 * font_gdi.c - the two gdi32 exports for memory fonts (plan 7), registered so
 * GetProcAddress(LoadLibraryA("gdi32.dll"), ..) finds them, as the game finds
 * them at 0x51F530. The fonts themselves are font_ct.m's.
 */
#include "win32hle.h"
#include "font.h"

#define ERROR_INVALID_PARAMETER 87u

/* AddFontMemResourceEx(pbFont, cbFont, pdv, pcFonts): a handle with *pcFonts set to the number of
 * faces, or NULL. pdv must be NULL, and the data and count pointers must be there
 * (ERROR_INVALID_PARAMETER); data that is not a font is NULL too, with its reason on stderr. */
static void k_AddFontMemResourceEx(void) {
    uint32_t data = A32(0), len = A32(1), pdv = A32(2), pcount = A32(3);
    if (pdv || !data || !len || !pcount || data + len < data) {
        hle_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0, 4);
    }
    uint32_t n = 0, h = bw2_font_add_memory((const uint8_t *)hle_ptr(data), len, &n);
    MEM32(pcount) = n;
    RET(h, 4);
}

/* RemoveFontMemResourceEx(h): TRUE, or FALSE with ERROR_INVALID_PARAMETER for an unknown handle. */
static void k_RemoveFontMemResourceEx(void) {
    if (bw2_font_remove_memory(A32(0))) RET(1, 1);
    hle_set_last_error(ERROR_INVALID_PARAMETER);
    RET(0, 1);
}

static const win32hle_shim g_exports[] = {
    { "AddFontMemResourceEx", k_AddFontMemResourceEx },
    { "RemoveFontMemResourceEx", k_RemoveFontMemResourceEx },
    { 0, 0 },
};
void bw2_font_register(void) { win32hle_register(g_exports); }
