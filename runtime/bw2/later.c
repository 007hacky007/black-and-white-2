/*
 * later.c - where plan 3's boot ends: the first call into a layer a later
 * plan owns (DirectInput before version 8, DirectSound/OpenAL, Bink, and
 * plan 5's D3DX shaders and effects). Each such import, and each LoadLibraryA of such a DLL, prints
 * "[bw2] reached <name>: owned by a later plan" and exits 4.
 *
 * The imports come from the images' own import tables (every name the game
 * imports from those DLLs), plus the entry points a game finds with
 * GetProcAddress.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "win32hle.h"
#include "bw2.h"

/* Plan 4 answers d3d9, ddraw and D3DX's math and textures; plan 5 owns D3DX's
 * shaders, effects, sprite, font and mesh (named below). */
static const char *const g_later_dlls[] = {
    "dinput.dll", "dsound.dll", "binkw32.dll", "openal32.dll", "wrap_oal.dll", NULL,
};
static const char *const g_later_names[] = {
    "DirectDrawCreate", "DirectDrawEnumerateExA", "DirectSoundCreate", "DirectSoundCreate8",
    "DirectSoundEnumerateA",
    "D3DXCompileShader", "D3DXCompileShaderFromFileA", "D3DXCreateFragmentLinker", "D3DXCreateSprite",
    "D3DXCreateFontA", "D3DXCreateSphere",
    NULL,
};

int bw2_later_dll(const char *base_name) {
    for (int i = 0; g_later_dlls[i]; i++)
        if (!strcasecmp(base_name, g_later_dlls[i])) return 1;
    return 0;
}

static void reached(const char *what) {
    fprintf(stderr, "[bw2] reached %s: owned by a later plan (from guest 0x%08X, sub_%08X)\n",
            what, MEM32(g_esp), g_cur_func);
    hle_exit(BW2_EXIT_LATER);
}
static void later_stop(void) { reached(g_cur_import ? g_cur_import : "?"); }
void bw2_later_load(const char *base_name) { reached(base_name); }

/* Register a stop for each name in g_later_names and each import from a later DLL in the PE at path. */
void bw2_later_register(const char *path) {
    static win32hle_shim shims[512];
    int n = 0;
    for (int i = 0; g_later_names[i]; i++) shims[n].name = g_later_names[i], shims[n++].fn = later_stop;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    uint8_t *d = malloc((size_t)len);
    fseek(f, 0, SEEK_SET);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return; }
    fclose(f);
    uint32_t nt = *(uint32_t *)(d + 0x3C), nsec = *(uint16_t *)(d + nt + 6);
    uint32_t sh = nt + 24 + *(uint16_t *)(d + nt + 20), imp = *(uint32_t *)(d + nt + 0x80);
    #define OFF(rva) ({ uint32_t _r = (rva), _o = 0; for (uint32_t _s = 0; _s < nsec; _s++) { \
        uint32_t *_h = (uint32_t *)(d + sh + 40 * _s); \
        if (_r >= _h[3] && _r < _h[3] + (_h[2] > _h[4] ? _h[2] : _h[4])) { _o = _r - _h[3] + _h[5]; break; } } _o; })
    for (uint32_t desc = imp ? OFF(imp) : 0; desc && *(uint32_t *)(d + desc + 12); desc += 20) {
        const char *dll = (const char *)d + OFF(*(uint32_t *)(d + desc + 12));
        if (!bw2_later_dll(dll)) continue;
        uint32_t thunk = *(uint32_t *)(d + desc) ? *(uint32_t *)(d + desc) : *(uint32_t *)(d + desc + 16);
        for (uint32_t t = OFF(thunk); *(uint32_t *)(d + t) && n < 511; t += 4) {
            uint32_t v = *(uint32_t *)(d + t);
            if (v & 0x80000000u) continue;                      /* by ordinal: none in these DLLs */
            shims[n].name = strdup((const char *)d + OFF(v) + 2);
            shims[n++].fn = later_stop;
        }
    }
    #undef OFF
    shims[n].name = NULL, shims[n].fn = NULL;
    free(d);
    win32hle_register(shims);
}
