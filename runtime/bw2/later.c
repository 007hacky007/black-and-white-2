/*
 * later.c - where plan 3's boot ends: the first call into a layer a later
 * plan owns (DirectInput before version 8, DirectSound/OpenAL, Bink, and
 * plan 5's D3DX shaders and effects). Each such import, and each LoadLibraryA of such a DLL, prints
 * "[bw2] reached <name>: owned by a later plan" and exits 4, as does
 * CoCreateInstance of a DirectSound class.
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

/* Plan 4 answers d3d9, ddraw and D3DX's math and textures, plan 7 its sprite;
 * later plans own D3DX's font, HLSL compiler, fragment linker and mesh (named below). */
static const char *const g_later_dlls[] = {
    "dinput.dll", "dsound.dll", "binkw32.dll", "openal32.dll", "wrap_oal.dll", NULL,
};
static const char *const g_later_names[] = {
    "DirectDrawCreate", "DirectDrawEnumerateExA", "DirectSoundCreate", "DirectSoundCreate8",
    "DirectSoundEnumerateA",
    "D3DXCompileShader", "D3DXCompileShaderFromFileA", "D3DXCreateFragmentLinker", "D3DXCreateFontA",
    "D3DXCreateSphere",
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

/* LoadLibraryA of a later plan's DLL stops, even one win32hle answers as a
 * system DLL (dsound.dll). The base name follows Windows' default extension
 * rule, as win32hle's own lookup does: no '.' gets ".dll", a trailing '.'
 * means none. Every other call is win32hle's. */
static void k_LoadLibraryA(void) {
    const char *name = ASTR(0);
    if (name) {
        const char *b = name + strlen(name);
        while (b > name && b[-1] != '\\' && b[-1] != '/' && b[-1] != ':') b--;
        char base[64];
        size_t k = strlen(b);
        if (k && b[k - 1] == '.') snprintf(base, sizeof base, "%.*s", (int)(k - 1), b);
        else snprintf(base, sizeof base, strchr(b, '.') ? "%s" : "%s.dll", b);
        if (bw2_later_dll(base)) {
            for (char *c = base; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
            reached(base);
        }
    }
    bw2_hle_shim("LoadLibraryA")();
}
const win32hle_shim bw2_later_shims[] = { { "LoadLibraryA", k_LoadLibraryA }, { NULL, NULL } };

/* The audio layer (plan 8 decides DirectSound or OpenAL): CoCreateInstance of
 * a DirectSound class stops. */
#define GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { (a) & 0xFF, (a) >> 8 & 0xFF, (a) >> 16 & 0xFF, (a) >> 24 & 0xFF, \
    (b) & 0xFF, (b) >> 8 & 0xFF, (c) & 0xFF, (c) >> 8 & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t g_dsound_clsids[][16] = {
    GUID(0x47D4D946u, 0x62E8, 0x11CF, 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00),   /* CLSID_DirectSound */
    GUID(0x3901CC3Fu, 0x84B5, 0x4FA4, 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B),   /* CLSID_DirectSound8 */
    GUID(0xB0210780u, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16),   /* CLSID_DirectSoundCapture */
    GUID(0xE4BCAC13u, 0x7F99, 0x4908, 0x9A, 0x8E, 0x74, 0xE3, 0xBF, 0x24, 0xB6, 0xE1),   /* CLSID_DirectSoundCapture8 */
    GUID(0xFEA4300Cu, 0x7959, 0x4147, 0xB2, 0x6A, 0x23, 0x77, 0xB9, 0xE7, 0xA9, 0x1D),   /* CLSID_DirectSoundFullDuplex */
};
#undef GUID
static uint32_t dsound_create(const uint8_t *iid, uint32_t *out) {
    (void)iid, (void)out;
    reached("DirectSound (CoCreateInstance)");
    return 0x80004005u;                                  /* not reached: reached() exits */
}
void bw2_later_classes(void) {
    for (size_t i = 0; i < sizeof g_dsound_clsids / sizeof *g_dsound_clsids; i++)
        hle_com_register_class(g_dsound_clsids[i], dsound_create);
}

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
