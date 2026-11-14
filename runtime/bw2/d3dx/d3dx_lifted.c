/*
 * d3dx_lifted.c - the disc's own d3dx9_25.dll, lifted with the game (plan 9).
 *
 * The rebased DLL sits beside the engine binary (the build copies it to
 * out/d3dx9_25.dll), or in Contents/Resources when the engine is an app's
 * executable (app.c bw2_app_resource; codesign refuses it in MacOS). The first call that needs it maps it at 0x08000000, binds
 * its imports, runs its DllMain (dll.c bw2_lifted_dll_open) and registers it as
 * bw2-d3dx9_25-lifted.dll, so the game's own d3dx9_25.dll lookups keep finding
 * the native D3DX. D3DXCompileShader, D3DXCompileShaderFromFileA,
 * D3DXCreateFragmentLinker and D3DXCreateSphere forward the game's arguments as
 * they are to the lifted exports; HLSL effect text goes to the lifted effect
 * compiler from fx_effect.c (plan 9 Task 5).
 *
 * D3DX's run-time questions to d3d9.dll are answered on purpose (Ruling 12):
 * Direct3DShaderValidatorCreate9 gives a validator that accepts every shader,
 * and DebugSetMute does nothing. The validator's methods and their argument
 * counts were read from the lifted DLL's code (docs/research/hlsl.md), call sites:
 *   create (no arguments)  0x080F4316, 0x08149B0D, 0x08165BAD
 *   slot 3 Begin(cb, ud, flags)                 0x080F4329, 0x08149B24, 0x08165BC3
 *   slot 4 Instruction(file, line, tok, count)  0x080F181E, 0x08143976, 0x08165BD9, 0x08165C4A, 0x08165C96
 *   slot 5 End()                                0x080F43FD, 0x0814A4EB, 0x08165CA2
 *   slot 2 Release()                            0x080F443A, 0x08149AD0, 0x08165CCE
 * DebugSetMute is called as cdecl with one argument (0x08029C12, 0x08029C21),
 * so it pops only its return address.
 */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "d3d_com.h"
#include "d3dx.h"

#define LIFTED_BASE 0x08000000u
#define LIFTED_SIZE 0x00253000u
#define LIFTED_MODULE "bw2-d3dx9_25-lifted.dll"

uint32_t bw2_lifted_dll_open(const char *module_name, const char *host_path, uint32_t base);   /* dll.c */
int bw2_app_resource(const char *name, char *out, size_t n);                                    /* app.c */

static char g_path[PATH_MAX];
static uint32_t g_hmod;
static int g_loading;

static void lifted_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void lifted_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bw2] FATAL: d3dx9_25.dll (lifted): ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (%s)\n", g_path);
    va_end(ap);
    fflush(stderr);
    abort();
}

/* d3dx9_25.dll in the bundle's Resources, or in the engine binary's own folder. */
static void find_dll(void) {
    if (bw2_app_resource("d3dx9_25.dll", g_path, sizeof g_path) != 0) {
        snprintf(g_path, sizeof g_path, "d3dx9_25.dll");
        lifted_fatal("cannot find the engine binary's folder");
    }
}

/* The PE's ImageBase and SizeOfImage, or a loud stop. */
static void check_header(void) {
    FILE *f = fopen(g_path, "rb");
    if (!f) lifted_fatal("no such file");
    uint8_t h[0x400];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    uint32_t lfanew = got >= 0x40 ? *(uint32_t *)(h + 0x3C) : 0;
    if (got < 0x40 || h[0] != 'M' || h[1] != 'Z' || lfanew > got - 0x58 || memcmp(h + lfanew, "PE\0\0", 4))
        lifted_fatal("not a PE file");
    uint32_t base = *(uint32_t *)(h + lfanew + 0x18 + 0x1C), size = *(uint32_t *)(h + lfanew + 0x18 + 0x38);
    if (base != LIFTED_BASE) lifted_fatal("ImageBase 0x%08X, not 0x%08X", base, LIFTED_BASE);
    if (size != LIFTED_SIZE) lifted_fatal("SizeOfImage 0x%08X, not 0x%08X", size, LIFTED_SIZE);
}

/* An export of the mapped image by name, or 0. */
static uint32_t export_of(uint32_t base, const char *name) {
    uint32_t nt = base + MEM32(base + 0x3C);
    uint32_t dir = MEM32(nt + 0x78);
    if (!dir) return 0;
    uint32_t d = base + dir, nfunc = MEM32(d + 20), nname = MEM32(d + 24);
    uint32_t funcs = base + MEM32(d + 28), names = base + MEM32(d + 32), ords = base + MEM32(d + 36);
    for (uint32_t k = 0; k < nname; k++)
        if (!strcmp((const char *)hle_ptr(base + MEM32(names + 4 * k)), name)) {
            uint32_t idx = MEM16(ords + 2 * k);
            return idx < nfunc && MEM32(funcs + 4 * idx) ? base + MEM32(funcs + 4 * idx) : 0;
        }
    return 0;
}

/* The lifted export's guest address, loading the DLL on the first call. Under the machine lock. */
uint32_t d3dx_lifted_proc(const char *export_name) {
    while (g_loading) hle_yield();                       /* another thread's DllMain is running */
    if (!g_hmod) {
        g_loading = 1;
        find_dll();
        check_header();
        g_hmod = bw2_lifted_dll_open(LIFTED_MODULE, g_path, LIFTED_BASE);
        g_loading = 0;
        if (!g_hmod) lifted_fatal("DllMain returned FALSE");
    }
    uint32_t va = export_of(g_hmod, export_name);
    if (!va) lifted_fatal("no export %s", export_name);
    return va;
}

/* ---- the forwarders ---- */
int d3dx_hlsl_log(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("BW2_HLSL_LOG"); on = e && *e && strcmp(e, "0"); }
    return on;
}
static void log_compile(uint32_t profile, uint32_t bytes, uint32_t hr, uint32_t pshader) {
    uint32_t tokens = 0, buf = pshader ? MEM32(pshader) : 0;
    if (!hr && buf) tokens = hle_call_guest(MEM32(MEM32(buf) + 16u), 1, &buf) / 4;   /* GetBufferSize */
    char prof[16] = "?";
    if (profile) snprintf(prof, sizeof prof, "%.15s", (const char *)hle_ptr(profile));
    for (char *c = prof; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '?';
    fprintf(stderr, "[bw2] hlsl shader %s: %u bytes -> 0x%08X, %u tokens\n", prof, bytes, hr, tokens);
}
/* (src, len, defines, include, function, profile, flags, ppShader, ppErrors, ppConstantTable) */
static void x_D3DXCompileShader(void) {
    uint32_t a[10];
    for (int i = 0; i < 10; i++) a[i] = A32(i);
    uint32_t hr = hle_call_guest(d3dx_lifted_proc("D3DXCompileShader"), 10, a);
    if (d3dx_hlsl_log()) log_compile(a[5], a[1], hr, a[7]);
    RET(hr, 10);
}
/* (file, defines, include, function, profile, flags, ppShader, ppErrors, ppConstantTable) */
static void x_D3DXCompileShaderFromFileA(void) {
    uint32_t a[9];
    for (int i = 0; i < 9; i++) a[i] = A32(i);
    uint32_t hr = hle_call_guest(d3dx_lifted_proc("D3DXCompileShaderFromFileA"), 9, a);
    if (d3dx_hlsl_log()) {
        char host[1024];
        struct stat st;
        uint32_t bytes = a[0] && hle_host_path((const char *)hle_ptr(a[0]), host, sizeof host) && !stat(host, &st)
                         ? (uint32_t)st.st_size : 0;
        log_compile(a[4], bytes, hr, a[6]);
    }
    RET(hr, 9);
}

/* (device, ShaderCacheSize, ID3DXFragmentLinker**): the lifted linker, which creates its shaders on our device */
static void x_D3DXCreateFragmentLinker(void) {
    uint32_t a[3] = { A32(0), A32(1), A32(2) };
    RET(hle_call_guest(d3dx_lifted_proc("D3DXCreateFragmentLinker"), 3, a), 3);
}
/* (device, FLOAT radius, slices, stacks, ID3DXMesh**, ID3DXBuffer** adjacency): the radius travels as its dword */
static void x_D3DXCreateSphere(void) {
    uint32_t a[6];
    for (int i = 0; i < 6; i++) a[i] = A32(i);
    RET(hle_call_guest(d3dx_lifted_proc("D3DXCreateSphere"), 6, a), 6);
}

/* ---- the shader validator (Ruling 12) ---- */
#define VAL_SLOTS 10                                     /* six methods, then four that stop */
#define VAL_CALLERS 16
static uint32_t g_val_vt, g_val_created, g_val_calls[VAL_SLOTS], g_mute_calls;
static uint32_t g_val_callers[VAL_SLOTS][VAL_CALLERS];
static int g_val_ncallers[VAL_SLOTS];

/* Creations (slot < 0) or calls of one slot, for the checks. */
uint32_t bw2_validator_calls(int slot) {
    if (slot < 0) return g_val_created;
    return slot < VAL_SLOTS ? g_val_calls[slot] : 0;
}
/* The distinct lifted functions one slot was called from (g_cur_func: lifted calls push no real
 * return address). */
int bw2_validator_callers(int slot, const uint32_t **out) {
    if (slot < 0 || slot >= VAL_SLOTS) return 0;
    *out = g_val_callers[slot];
    return g_val_ncallers[slot];
}
uint32_t bw2_debugsetmute_calls(void) { return g_mute_calls; }

static void count(int slot) {
    uint32_t from = g_cur_func;
    g_val_calls[slot]++;
    for (int i = 0; i < g_val_ncallers[slot]; i++) if (g_val_callers[slot][i] == from) return;
    if (g_val_ncallers[slot] < VAL_CALLERS) g_val_callers[slot][g_val_ncallers[slot]++] = from;
}
static void v_QueryInterface(void) {                     /* (this, riid, ppv): IUnknown only */
    static const uint8_t iid_unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    count(0);
    uint32_t self = A32(0), riid = A32(1), ppv = A32(2);
    if (!ppv) RET(0x80004003u, 3);                       /* E_POINTER */
    if (riid && !memcmp(hle_ptr(riid), iid_unknown, 16)) {
        MEM32(self + 4)++;
        MEM32(ppv) = self;
        RET(D3D_OK, 3);
    }
    MEM32(ppv) = 0;
    RET(E_NOINTERFACE_, 3);
}
static void v_AddRef(void) { count(1); RET(++MEM32(A32(0) + 4), 1); }
static void v_Release(void) {
    count(2);
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) hle_com_free(self);
    RET(n, 1);
}
static void v_Begin(void) { count(3); RET(D3D_OK, 4); }          /* (this, callback, user, flags) */
static void v_Instruction(void) { count(4); RET(D3D_OK, 5); }    /* (this, file, line, tokens, count) */
static void v_End(void) { count(5); RET(D3D_OK, 1); }
static void v_unknown(int slot) {
    fprintf(stderr, "[bw2] FATAL: shader validator slot %d (from 0x%08X)\n", slot, g_cur_func);
    fflush(stderr);
    abort();
}
static void v_slot6(void) { v_unknown(6); }
static void v_slot7(void) { v_unknown(7); }
static void v_slot8(void) { v_unknown(8); }
static void v_slot9(void) { v_unknown(9); }
/* each entry its own name: entries sharing one resolve to the first */
static const win32hle_shim g_val_methods[] = {
    { "IDirect3DShaderValidator9::Slot0", v_QueryInterface }, { "IDirect3DShaderValidator9::Slot1", v_AddRef },
    { "IDirect3DShaderValidator9::Slot2", v_Release }, { "IDirect3DShaderValidator9::Slot3", v_Begin },
    { "IDirect3DShaderValidator9::Slot4", v_Instruction }, { "IDirect3DShaderValidator9::Slot5", v_End },
    { "IDirect3DShaderValidator9::Slot6", v_slot6 }, { "IDirect3DShaderValidator9::Slot7", v_slot7 },
    { "IDirect3DShaderValidator9::Slot8", v_slot8 }, { "IDirect3DShaderValidator9::Slot9", v_slot9 },
    { 0, 0 } };

static void x_Direct3DShaderValidatorCreate9(void) {     /* (): a new validator, one reference */
    if (!g_val_vt) g_val_vt = hle_com_vtable(g_val_methods);
    g_val_created++;
    RET(hle_com_new(g_val_vt, 8), 0);
}
/* d3d9.dll's DebugSetMute: the debug runtime's output switch; the retail runtime has nothing to mute. */
static void x_DebugSetMute(void) { g_mute_calls++; RET(0, 0); }

static const win32hle_shim g_exports[] = {
    { "D3DXCompileShader", x_D3DXCompileShader },
    { "D3DXCompileShaderFromFileA", x_D3DXCompileShaderFromFileA },
    { "D3DXCreateFragmentLinker", x_D3DXCreateFragmentLinker },
    { "D3DXCreateSphere", x_D3DXCreateSphere },
    { "Direct3DShaderValidatorCreate9", x_Direct3DShaderValidatorCreate9 },
    { "DebugSetMute", x_DebugSetMute },
    { 0, 0 } };
void d3dx_lifted_register(void) { win32hle_register(g_exports); }
