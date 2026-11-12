/*
 * fx_selftest.c - D3DX effects (plan 5 Task 5): the fx_2_0 binary, the
 * parameter methods and effect pools, called as lifted code calls them, on a
 * headless device. Built with the engine (minus main.c) by
 * tests/test_effects.py; `fx_selftest <group> [args]` runs one group and
 * prints "<group>: <n> failed". The effects are written by tests/fxwrite.py;
 * the expectations below follow the effects test_effects.py builds.
 */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "fx.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
/* expressions, so checks can be chained with commas */
static int chk(int ok, const char *what, int line) {
    if (!ok) { printf("FAIL %s:%d %s\n", __FILE__, line, what); fails++; }
    return ok;
}
static int chk_eq(uint64_t a, uint64_t b, const char *ea, const char *eb, int line) {
    if (a != b) {
        printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, line, ea, eb, (unsigned long long)a,
               (unsigned long long)b);
        fails++;
    }
    return a == b;
}
static int chk_f(float a, float b, const char *ea, const char *eb, int line) {
    if (!(a == b)) { printf("FAIL %s:%d %s == %s: %g != %g\n", __FILE__, line, ea, eb, a, b); fails++; }
    return a == b;
}
#define CHECK(c) chk(!!(c), #c, __LINE__)
#define CHECKEQ(a, b) chk_eq((uint64_t)(a), (uint64_t)(b), #a, #b, __LINE__)
#define CHECKF(a, b) chk_f((a), (b), #a, #b, __LINE__)

static uint32_t run(recomp_func_t f, const char *name, int n, const uint32_t *a) {
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    if (g_esp != esp0) {
        printf("FAIL %s: popped %d bytes, want %d\n", name, (int)(g_esp - (esp0 - 4u - 4u * (uint32_t)n)), 4 + 4 * n);
        fails++;
        g_esp = esp0;
    }
    return g_eax;
}
static uint32_t call(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, name, n, a);
}
static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, g_cur_import, n + 1, a);
}
static uint32_t gmem(uint32_t n) { uint32_t p = hle_va(hle_alloc(n)); memset(hle_ptr(p), 0, n); return p; }
static uint32_t gstr(const char *s) { uint32_t p = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(p), s); return p; }
static uint32_t gbytes(const void *b, uint32_t n) { uint32_t p = gmem(n + 1); memcpy(hle_ptr(p), b, n); return p; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float gf(uint32_t p) { return u2f(MEM32(p)); }

static uint8_t *read_file(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); fails++; *n = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len + 1);
    *n = (uint32_t)fread(d, 1, (size_t)len, f);
    fclose(f);
    return d;
}

/* ID3DXEffect slots (d3dx9_25's order) */
enum { FX_QueryInterface, FX_AddRef, FX_Release, FX_GetDesc, FX_GetParameterDesc, FX_GetTechniqueDesc,
       FX_GetPassDesc, FX_GetFunctionDesc, FX_GetParameter, FX_GetParameterByName, FX_GetParameterBySemantic,
       FX_GetParameterElement, FX_GetTechnique, FX_GetTechniqueByName, FX_GetPass, FX_GetPassByName,
       FX_GetFunction, FX_GetFunctionByName, FX_GetAnnotation, FX_GetAnnotationByName, FX_SetValue, FX_GetValue,
       FX_SetBool, FX_GetBool, FX_SetBoolArray, FX_GetBoolArray, FX_SetInt, FX_GetInt, FX_SetIntArray,
       FX_GetIntArray, FX_SetFloat, FX_GetFloat, FX_SetFloatArray, FX_GetFloatArray, FX_SetVector, FX_GetVector,
       FX_SetVectorArray, FX_GetVectorArray, FX_SetMatrix, FX_GetMatrix, FX_SetMatrixArray, FX_GetMatrixArray,
       FX_SetMatrixPointerArray, FX_GetMatrixPointerArray, FX_SetMatrixTranspose, FX_GetMatrixTranspose,
       FX_SetMatrixTransposeArray, FX_GetMatrixTransposeArray, FX_SetMatrixTransposePointerArray,
       FX_GetMatrixTransposePointerArray, FX_SetString, FX_GetString, FX_SetTexture, FX_GetTexture,
       FX_GetPixelShader, FX_GetVertexShader, FX_SetArrayRange, FX_GetPool, FX_SetTechnique,
       FX_GetCurrentTechnique, FX_ValidateTechnique, FX_FindNextValidTechnique, FX_IsParameterUsed, FX_Begin,
       FX_BeginPass, FX_CommitChanges, FX_EndPass, FX_End, FX_GetDevice, FX_OnLostDevice, FX_OnResetDevice,
       FX_SetStateManager, FX_GetStateManager, FX_BeginParameterBlock, FX_EndParameterBlock,
       FX_ApplyParameterBlock, FX_CloneEffect };
/* D3DXPARAMETER_DESC */
enum { PD_Name = 0, PD_Semantic = 4, PD_Class = 8, PD_Type = 12, PD_Rows = 16, PD_Columns = 20, PD_Elements = 24,
       PD_Annotations = 28, PD_StructMembers = 32, PD_Flags = 36, PD_Bytes = 40 };

static uint32_t new_device(void) {
    static uint32_t d3d;
    if (!d3d) d3d = call("Direct3DCreate9", 1, 32u);
    uint32_t pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = 64, MEM32(pp + 4) = 64, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1, MEM32(pp + 36) = 1, MEM32(pp + 40) = 75;
    CHECKEQ(vcall(d3d, 16, 6, 0u, 1u, 0x10001u, 0x40u, pp, out), D3D_OK);
    return MEM32(out);
}
static uint32_t refs(uint32_t o) { vcall(o, 1, 0); return vcall(o, 2, 0); }

static const char *gs(uint32_t p) { return p ? (const char *)hle_ptr(p) : "(null)"; }

/* D3DXCreateEffect on the bytes; *hr gets its result */
static uint32_t create(uint32_t dev, const uint8_t *bin, uint32_t len, uint32_t pool, uint32_t *hr) {
    uint32_t out = gmem(4), errs = gmem(4);
    MEM32(out) = 0xCDCDCDCDu, MEM32(errs) = 0xCDCDCDCDu;
    uint32_t r = call("D3DXCreateEffect", 9, dev, gbytes(bin, len), len, 0u, 0u, 0u, pool, out, errs);
    if (hr) *hr = r;
    else CHECKEQ(r, D3D_OK);
    if (r == D3D_OK) CHECKEQ(MEM32(errs), 0);
    else {
        CHECKEQ(MEM32(out), 0);
        uint32_t b = MEM32(errs);                                 /* the message, which the caller frees */
        if (b && b != 0xCDCDCDCDu) printf("errors: %s\n", gs(vcall(b, 3, 0))), vcall(b, 2, 0);
    }
    return r == D3D_OK ? MEM32(out) : 0;
}
static uint32_t create_file(uint32_t dev, const char *path, uint32_t pool) {
    uint32_t n;
    uint8_t *d = read_file(path, &n);
    uint32_t e = d ? create(dev, d, n, pool, NULL) : 0;
    free(d);
    return e;
}
static uint32_t byname(uint32_t e, const char *name) { return vcall(e, FX_GetParameterByName, 2, 0u, gstr(name)); }

/* ---- fx_params: one of each parameter type (test_effects.py's params_effect) ---- */

static void group_fx_params(const char *path) {
    uint32_t dev = new_device(), live0 = d3d_live_count();
    uint32_t e = create_file(dev, path, 0);
    if (!e) return;
    uint32_t desc = gmem(64), out = gmem(64), v4 = gmem(16);
    CHECKEQ(vcall(e, FX_GetDesc, 1, desc), D3D_OK);
    CHECKEQ(MEM32(desc + 4), 12);                                   /* Parameters */
    CHECKEQ(MEM32(desc + 8), 1);                                    /* Techniques */
    CHECKEQ(MEM32(desc + 12), 0);                                   /* Functions */
    static const char *const names[] = { "b", "i", "f", "v", "v3", "m", "t", "s", "arr", "str", "smp", "fa" };
    static const uint32_t want[12][9] = {          /* class, type, rows, columns, elements, annos, members, bytes */
        { 0, 1, 1, 1, 0, 0, 0, 4 }, { 0, 2, 1, 1, 0, 0, 0, 4 }, { 0, 3, 1, 1, 0, 2, 0, 4 }, { 1, 3, 1, 4, 0, 0, 0, 16 },
        { 1, 3, 1, 3, 0, 0, 0, 12 }, { 2, 3, 4, 4, 0, 0, 0, 64 }, { 4, 5, 0, 0, 0, 0, 0, 4 }, { 5, 0, 0, 0, 0, 0, 2, 20 },
        { 1, 3, 1, 4, 4, 0, 0, 64 }, { 4, 4, 0, 0, 0, 0, 0, 4 }, { 4, 12, 0, 0, 0, 0, 0, 0 }, { 0, 3, 1, 1, 3, 0, 0, 12 } };
    uint32_t h[12];
    for (int i = 0; i < 12; i++) {
        h[i] = byname(e, names[i]);
        CHECK(h[i] != 0);
        CHECKEQ(vcall(e, FX_GetParameter, 2, 0u, (uint32_t)i), h[i]);
        CHECKEQ(vcall(e, FX_GetParameterDesc, 2, h[i], desc), D3D_OK);
        if (strcmp(gs(MEM32(desc + PD_Name)), names[i])) { printf("FAIL desc name %s: %s\n", names[i], gs(MEM32(desc))); fails++; }
        CHECKEQ(MEM32(desc + PD_Class), want[i][0]);
        CHECKEQ(MEM32(desc + PD_Type), want[i][1]);
        CHECKEQ(MEM32(desc + PD_Rows), want[i][2]);
        CHECKEQ(MEM32(desc + PD_Columns), want[i][3]);
        CHECKEQ(MEM32(desc + PD_Elements), want[i][4]);
        CHECKEQ(MEM32(desc + PD_Annotations), want[i][5]);
        CHECKEQ(MEM32(desc + PD_StructMembers), want[i][6]);
        CHECKEQ(MEM32(desc + PD_Bytes), want[i][7]);
        CHECKEQ(MEM32(desc + PD_Flags), 0);
    }
    CHECKEQ(vcall(e, FX_GetParameter, 2, 0u, 12u), 0);
    CHECKEQ(byname(e, "nope"), 0);
    CHECK(!strcmp(gs(MEM32(desc + PD_Semantic)), "(null)"));
    CHECKEQ(vcall(e, FX_GetParameterDesc, 2, h[2], desc), D3D_OK);
    CHECK(!strcmp(gs(MEM32(desc + PD_Semantic)), "FSEM"));
    CHECKEQ(vcall(e, FX_GetParameterBySemantic, 2, 0u, gstr("fsem")), h[2]);   /* semantics ignore case */
    CHECKEQ(vcall(e, FX_GetParameterBySemantic, 2, 0u, gstr("NONE")), 0);

    /* the defaults */
    CHECKEQ(vcall(e, FX_GetBool, 2, h[0], out), D3D_OK), CHECKEQ(MEM32(out), 1);
    CHECKEQ(vcall(e, FX_GetInt, 2, h[1], out), D3D_OK), CHECKEQ(MEM32(out), 7);
    CHECKEQ(vcall(e, FX_GetFloat, 2, h[2], out), D3D_OK), CHECKF(gf(out), 0.5f);
    CHECKEQ(vcall(e, FX_GetVector, 2, h[3], out), D3D_OK);
    for (int k = 0; k < 4; k++) CHECKF(gf(out + 4 * k), (float)(k + 1));
    CHECKEQ(vcall(e, FX_GetVector, 2, h[4], out), D3D_OK);           /* float3: w reads 0 */
    CHECKF(gf(out), 0.25f), CHECKF(gf(out + 4), 0.5f), CHECKF(gf(out + 8), 0.75f), CHECKF(gf(out + 12), 0);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, h[5], out), D3D_OK);
    for (int k = 0; k < 16; k++) CHECKF(gf(out + 4 * k), (float)k);
    MEM32(out) = 0xCDCDCDCDu;
    CHECKEQ(vcall(e, FX_GetTexture, 2, h[6], out), D3D_OK), CHECKEQ(MEM32(out), 0);
    CHECKEQ(vcall(e, FX_GetValue, 3, h[7], out, 20u), D3D_OK);
    CHECKF(gf(out), 1), CHECKF(gf(out + 4), 5), CHECKF(gf(out + 8), 6), CHECKF(gf(out + 12), 7), CHECKF(gf(out + 16), 8);
    CHECKEQ(vcall(e, FX_GetVectorArray, 3, h[8], out, 4u), D3D_OK);
    for (int k = 0; k < 4; k++) {
        CHECKF(gf(out + 16 * k), (float)k), CHECKF(gf(out + 16 * k + 4), k + 0.5f);
        CHECKF(gf(out + 16 * k + 8), k + 0.25f), CHECKF(gf(out + 16 * k + 12), (float)-k);
    }
    CHECKEQ(vcall(e, FX_GetString, 2, h[9], out), D3D_OK);
    CHECK(!strcmp(gs(MEM32(out)), "text"));
    CHECKEQ(vcall(e, FX_GetFloatArray, 3, h[11], out, 3u), D3D_OK);
    CHECKF(gf(out), 1.5f), CHECKF(gf(out + 4), 2.5f), CHECKF(gf(out + 8), 3.5f);

    /* each Set, then Get */
    CHECKEQ(vcall(e, FX_SetBool, 2, h[0], 0u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetBool, 2, h[0], out), D3D_OK), CHECKEQ(MEM32(out), 0);
    CHECKEQ(vcall(e, FX_SetBool, 2, h[0], 5u), D3D_OK);              /* a bool reads 0/1 */
    CHECKEQ(vcall(e, FX_GetBool, 2, h[0], out), D3D_OK), CHECKEQ(MEM32(out), 1);
    CHECKEQ(vcall(e, FX_GetInt, 2, h[0], out), D3D_OK), CHECKEQ(MEM32(out), 1);
    CHECKEQ(vcall(e, FX_SetInt, 2, h[1], (uint32_t)-3), D3D_OK);
    CHECKEQ(vcall(e, FX_GetInt, 2, h[1], out), D3D_OK), CHECKEQ(MEM32(out), (uint32_t)-3);
    CHECKEQ(vcall(e, FX_SetFloat, 2, h[1], f2u(2.75f)), D3D_OK);     /* a float set on an int truncates */
    CHECKEQ(vcall(e, FX_GetInt, 2, h[1], out), D3D_OK), CHECKEQ(MEM32(out), 2);
    CHECKEQ(vcall(e, FX_SetFloat, 2, h[1], f2u(-2.75f)), D3D_OK);
    CHECKEQ(vcall(e, FX_GetInt, 2, h[1], out), D3D_OK), CHECKEQ(MEM32(out), (uint32_t)-2);
    CHECKEQ(vcall(e, FX_GetFloat, 2, h[1], out), D3D_OK), CHECKF(gf(out), -2.0f);
    CHECKEQ(vcall(e, FX_SetFloat, 2, h[2], f2u(-1.25f)), D3D_OK);
    CHECKEQ(vcall(e, FX_GetFloat, 2, h[2], out), D3D_OK), CHECKF(gf(out), -1.25f);
    CHECKEQ(vcall(e, FX_GetBool, 2, h[2], out), D3D_OK), CHECKEQ(MEM32(out), 1);
    CHECKEQ(vcall(e, FX_SetBool, 2, h[2], 1u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetFloat, 2, h[2], out), D3D_OK), CHECKF(gf(out), 1.0f);
    for (int k = 0; k < 4; k++) MEM32(v4 + 4 * k) = f2u(10.0f + (float)k);
    CHECKEQ(vcall(e, FX_SetVector, 2, h[3], v4), D3D_OK);
    CHECKEQ(vcall(e, FX_GetVector, 2, h[3], out), D3D_OK);
    for (int k = 0; k < 4; k++) CHECKF(gf(out + 4 * k), 10.0f + (float)k);
    CHECKEQ(vcall(e, FX_SetVector, 2, h[4], v4), D3D_OK);           /* float3 keeps xyz */
    CHECKEQ(vcall(e, FX_GetValue, 3, h[4], out, 12u), D3D_OK);
    CHECKF(gf(out), 10), CHECKF(gf(out + 4), 11), CHECKF(gf(out + 8), 12);
    CHECKEQ(vcall(e, FX_SetInt, 2, h[4], 0x80FF4000u), D3D_OK);     /* an int on a float3 is a colour */
    CHECKEQ(vcall(e, FX_GetVector, 2, h[4], out), D3D_OK);
    CHECKF(gf(out), 1.0f), CHECKF(gf(out + 4), 64 / 255.0f), CHECKF(gf(out + 8), 0);
    uint32_t m = gmem(64);
    for (int k = 0; k < 16; k++) MEM32(m + 4 * k) = f2u(100.0f + (float)k);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, h[5], m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, h[5], out), D3D_OK);
    for (int k = 0; k < 16; k++) CHECKF(gf(out + 4 * k), 100.0f + (float)k);
    MEM32(desc) = 0x11223344u;
    CHECKEQ(vcall(e, FX_SetValue, 3, h[7], gbytes((float[5]){ 9, 1, 2, 3, 4 }, 20), 20u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetValue, 3, h[7], out, 20u), D3D_OK);
    CHECKF(gf(out), 9), CHECKF(gf(out + 16), 4);
    CHECKEQ(vcall(e, FX_GetValue, 3, h[7], out, 16u), D3DERR_INVALIDCALL);   /* too small */
    for (int k = 0; k < 16; k++) MEM32(m + 4 * k) = f2u(-(float)k);
    CHECKEQ(vcall(e, FX_SetVectorArray, 3, h[8], m, 4u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetVectorArray, 3, h[8], out, 4u), D3D_OK);
    for (int k = 0; k < 16; k++) CHECKF(gf(out + 4 * k), -(float)k);
    CHECKEQ(vcall(e, FX_SetVectorArray, 3, h[8], m, 5u), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetString, 2, h[9], gstr("other")), D3D_OK);
    CHECKEQ(vcall(e, FX_GetString, 2, h[9], out), D3D_OK);
    CHECK(!strcmp(gs(MEM32(out)), "other"));
    CHECKEQ(vcall(e, FX_SetFloatArray, 3, h[11], gbytes((float[3]){ 7, 8, 9 }, 12), 3u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetFloatArray, 3, h[11], out, 3u), D3D_OK);
    CHECKF(gf(out), 7), CHECKF(gf(out + 4), 8), CHECKF(gf(out + 8), 9);
    CHECKEQ(vcall(e, FX_SetIntArray, 3, h[11], gbytes((int32_t[2]){ 4, -5 }, 8), 2u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetIntArray, 3, h[11], out, 3u), D3D_OK);
    CHECKEQ(MEM32(out), 4), CHECKEQ(MEM32(out + 4), (uint32_t)-5), CHECKEQ(MEM32(out + 8), 9);
    CHECKEQ(vcall(e, FX_SetBoolArray, 3, h[11], gbytes((int32_t[3]){ 0, 3, 1 }, 12), 3u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetBoolArray, 3, h[11], out, 3u), D3D_OK);
    CHECKEQ(MEM32(out), 0), CHECKEQ(MEM32(out + 4), 1), CHECKEQ(MEM32(out + 8), 1);
    CHECKEQ(vcall(e, FX_GetFloatArray, 3, h[11], out, 3u), D3D_OK);     /* SetBoolArray takes ints (as D3DX) */
    CHECKF(gf(out), 0), CHECKF(gf(out + 4), 3), CHECKF(gf(out + 8), 1);

    /* by a name string */
    CHECKEQ(vcall(e, FX_SetFloat, 2, gstr("f"), f2u(2.5f)), D3D_OK);
    CHECKEQ(vcall(e, FX_GetFloat, 2, h[2], out), D3D_OK), CHECKF(gf(out), 2.5f);
    CHECKEQ(vcall(e, FX_GetFloat, 2, gstr("f"), out), D3D_OK), CHECKF(gf(out), 2.5f);
    CHECKEQ(vcall(e, FX_SetFloat, 2, gstr("nope"), f2u(1)), D3DERR_INVALIDCALL);
    /* "s.member", members and elements */
    uint32_t hm = byname(e, "s.member");
    CHECK(hm != 0);
    CHECKEQ(vcall(e, FX_GetParameter, 2, h[7], 1u), hm);
    CHECKEQ(vcall(e, FX_GetParameterByName, 2, h[7], gstr("member")), hm);
    CHECKEQ(vcall(e, FX_GetParameter, 2, h[7], 2u), 0);
    CHECKEQ(vcall(e, FX_GetParameterDesc, 2, hm, desc), D3D_OK);
    CHECK(!strcmp(gs(MEM32(desc + PD_Name)), "member"));
    CHECKEQ(vcall(e, FX_SetVector, 2, gstr("s.member"), v4), D3D_OK);
    CHECKEQ(vcall(e, FX_GetValue, 3, h[7], out, 20u), D3D_OK);
    CHECKF(gf(out), 9), CHECKF(gf(out + 4), 10), CHECKF(gf(out + 16), 13);
    CHECKEQ(vcall(e, FX_SetFloat, 2, gstr("s.a"), f2u(-4)), D3D_OK);
    CHECKEQ(vcall(e, FX_GetValue, 3, h[7], out, 20u), D3D_OK), CHECKF(gf(out), -4);
    uint32_t he = vcall(e, FX_GetParameterElement, 2, h[8], 2u);
    CHECK(he != 0);
    CHECKEQ(vcall(e, FX_GetParameterElement, 2, h[8], 4u), 0);
    CHECKEQ(vcall(e, FX_GetParameterElement, 2, h[3], 0u), 0);
    CHECKEQ(byname(e, "arr[2]"), he);
    CHECKEQ(vcall(e, FX_GetParameterDesc, 2, he, desc), D3D_OK);
    CHECK(!strcmp(gs(MEM32(desc + PD_Name)), "arr"));
    CHECKEQ(MEM32(desc + PD_Elements), 0), CHECKEQ(MEM32(desc + PD_Bytes), 16);
    CHECKEQ(vcall(e, FX_SetVector, 2, he, v4), D3D_OK);
    CHECKEQ(vcall(e, FX_GetVectorArray, 3, h[8], out, 4u), D3D_OK);
    CHECKF(gf(out + 32), 10), CHECKF(gf(out + 44), 13), CHECKF(gf(out + 16), -4);
    CHECKEQ(vcall(e, FX_SetArrayRange, 3, h[8], 1u, 3u), D3D_OK);
    CHECKEQ(vcall(e, FX_SetArrayRange, 3, h[8], 3u, 5u), D3DERR_INVALIDCALL);

    /* a type mismatch */
    CHECKEQ(vcall(e, FX_SetFloat, 2, h[3], f2u(1)), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_GetFloat, 2, h[3], out), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, h[2], m), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, h[3], m), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetVector, 2, h[5], v4), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetVector, 2, h[8], v4), D3DERR_INVALIDCALL);    /* an array */
    CHECKEQ(vcall(e, FX_SetTexture, 2, h[2], 0u), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_GetString, 2, h[2], out), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetString, 2, h[2], gstr("x")), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetBool, 2, h[7], 1u), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetFloatArray, 3, h[7], m, 2u), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetValue, 3, h[10], m, 4u), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetFloat, 2, 0u, f2u(1)), D3DERR_INVALIDCALL);

    /* annotations */
    uint32_t a1 = vcall(e, FX_GetAnnotation, 2, h[2], 0u), a2 = vcall(e, FX_GetAnnotationByName, 2, h[2], gstr("a2"));
    CHECK(a1 != 0 && a2 != 0 && a1 != a2);
    CHECKEQ(vcall(e, FX_GetAnnotation, 2, h[2], 1u), a2);
    CHECKEQ(vcall(e, FX_GetAnnotation, 2, h[2], 2u), 0);
    CHECKEQ(vcall(e, FX_GetString, 2, a1, out), D3D_OK);
    CHECK(!strcmp(gs(MEM32(out)), "hello"));
    CHECKEQ(vcall(e, FX_GetFloat, 2, a2, out), D3D_OK), CHECKF(gf(out), 3.0f);
    CHECKEQ(vcall(e, FX_GetParameterDesc, 2, a1, desc), D3D_OK);
    CHECK(!strcmp(gs(MEM32(desc + PD_Name)), "a1"));
    CHECKEQ(MEM32(desc + PD_Flags), 4u);                           /* D3DX_PARAMETER_ANNOTATION */
    CHECKEQ(byname(e, "f@a2"), a2);

    /* techniques and passes */
    uint32_t ht = vcall(e, FX_GetTechniqueByName, 1, gstr("t0"));
    CHECK(ht != 0);
    CHECKEQ(vcall(e, FX_GetTechnique, 1, 0u), ht);
    CHECKEQ(vcall(e, FX_GetTechnique, 1, 1u), 0);
    CHECKEQ(vcall(e, FX_GetTechniqueByName, 1, gstr("t1")), 0);
    CHECKEQ(vcall(e, FX_GetTechniqueDesc, 2, ht, desc), D3D_OK);
    CHECK(!strcmp(gs(MEM32(desc)), "t0"));
    CHECKEQ(MEM32(desc + 4), 1), CHECKEQ(MEM32(desc + 8), 1);
    uint32_t hp = vcall(e, FX_GetPass, 2, ht, 0u);
    CHECK(hp != 0);
    CHECKEQ(vcall(e, FX_GetPassByName, 2, ht, gstr("p0")), hp);
    CHECKEQ(vcall(e, FX_GetPass, 2, ht, 1u), 0);
    CHECKEQ(vcall(e, FX_GetPassDesc, 2, hp, desc), D3D_OK);
    CHECK(!strcmp(gs(MEM32(desc)), "p0"));
    CHECKEQ(MEM32(desc + 4), 1), CHECKEQ(MEM32(desc + 8), 0), CHECKEQ(MEM32(desc + 12), 0);
    uint32_t ta = vcall(e, FX_GetAnnotationByName, 2, ht, gstr("tanno"));
    CHECKEQ(vcall(e, FX_GetString, 2, ta, out), D3D_OK);
    CHECK(!strcmp(gs(MEM32(out)), "x"));
    uint32_t pa = vcall(e, FX_GetAnnotation, 2, hp, 0u);
    CHECKEQ(vcall(e, FX_GetInt, 2, pa, out), D3D_OK), CHECKEQ(MEM32(out), 2);
    CHECKEQ(vcall(e, FX_GetParameterDesc, 2, ht, desc), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_GetTechniqueDesc, 2, h[2], desc), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_GetFunction, 1, 0u), 0);

    /* a texture: Set AddRefs it, Get AddRefs it, a new Set and the effect's end release it */
    uint32_t to = gmem(4);
    CHECKEQ(vcall(dev, 23, 8, 4u, 4u, 1u, 0u, 21u, 1u, to, 0u), D3D_OK);        /* CreateTexture, managed */
    uint32_t tex = MEM32(to);
    CHECKEQ(vcall(e, FX_SetTexture, 2, h[6], tex), D3D_OK);
    CHECKEQ(refs(tex), 2);
    CHECKEQ(vcall(e, FX_GetTexture, 2, h[6], out), D3D_OK), CHECKEQ(MEM32(out), tex);
    CHECKEQ(refs(tex), 3);
    vcall(tex, 2, 0);
    CHECKEQ(vcall(e, FX_SetTexture, 2, gstr("t"), tex), D3D_OK);              /* the same one: no new reference */
    CHECKEQ(refs(tex), 2);
    CHECKEQ(vcall(e, FX_SetTexture, 2, h[6], 0u), D3D_OK);
    CHECKEQ(refs(tex), 1);
    CHECKEQ(vcall(e, FX_SetTexture, 2, h[6], tex), D3D_OK);
    CHECKEQ(vcall(e, FX_GetValue, 3, h[6], out, 4u), D3D_OK), CHECKEQ(MEM32(out), tex);   /* AddRefs too */
    CHECKEQ(refs(tex), 3);
    vcall(tex, 2, 0);

    /* CloneEffect (the game clones each effect it loads): the current values, its own from then on */
    uint32_t dev2 = new_device();
    CHECKEQ(vcall(e, FX_CloneEffect, 2, 0u, out), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_CloneEffect, 2, dev2, out), D3D_OK);
    uint32_t k = MEM32(out);
    CHECK(k != 0 && k != e);
    CHECKEQ(refs(tex), 3);                                         /* the clone's t holds one too */
    CHECKEQ(vcall(k, FX_GetFloat, 2, gstr("f"), out), D3D_OK), CHECKF(gf(out), 2.5f);
    CHECKEQ(vcall(k, FX_GetString, 2, gstr("str"), out), D3D_OK);
    CHECK(!strcmp(gs(MEM32(out)), "other"));
    CHECKEQ(vcall(k, FX_GetValue, 3, gstr("s"), out, 20u), D3D_OK), CHECKF(gf(out), -4);
    CHECKEQ(vcall(k, FX_SetFloat, 2, gstr("f"), f2u(8)), D3D_OK);
    CHECKEQ(vcall(e, FX_GetFloat, 2, gstr("f"), out), D3D_OK), CHECKF(gf(out), 2.5f);
    CHECKEQ(vcall(k, FX_GetDevice, 1, out), D3D_OK), CHECKEQ(MEM32(out), dev2);
    vcall(dev2, 2, 0);
    CHECKEQ(vcall(k, FX_GetTechniqueByName, 1, gstr("t0")) != 0, 1);
    CHECKEQ(vcall(k, FX_Release, 0), 0);
    CHECKEQ(refs(tex), 2);
    vcall(dev2, 2, 0);

    /* the device, and the end */
    CHECKEQ(vcall(e, FX_GetDevice, 1, out), D3D_OK), CHECKEQ(MEM32(out), dev);
    vcall(dev, 2, 0);
    CHECKEQ(vcall(e, FX_AddRef, 0), 2);
    CHECKEQ(vcall(e, FX_Release, 0), 1);
    CHECKEQ(vcall(e, FX_Release, 0), 0);
    CHECKEQ(refs(tex), 1);
    vcall(tex, 2, 0);
    CHECKEQ(d3d_live_count(), live0);
}

/* ---- fx_matrix: row- and column-major matrices (test_effects.py's matrix_effect) ---- */

static void mat_from(uint32_t p, float base) { for (int k = 0; k < 16; k++) MEM32(p + 4 * k) = f2u(base + (float)k); }
static int mat_is(uint32_t p, float base, int transposed, int rows, int cols) {
    int ok = 1;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int rr = transposed ? c : r, cc = transposed ? r : c;
            float want = (rr < rows && cc < cols) ? base + (float)(4 * rr + cc) : 0;
            if (gf(p + 4 * (4 * r + c)) != want) {
                if (ok) printf("FAIL matrix [%d][%d]: %g, want %g\n", r, c, gf(p + 4 * (4 * r + c)), want);
                ok = 0;
            }
        }
    if (!ok) fails++;
    return ok;
}

static void group_fx_matrix(const char *path) {
    uint32_t dev = new_device(), live0 = d3d_live_count();
    uint32_t e = create_file(dev, path, 0);
    if (!e) return;
    uint32_t mr = byname(e, "mr"), mc = byname(e, "mc"), m34 = byname(e, "m34"), c43 = byname(e, "c43");
    uint32_t ma = byname(e, "ma"), mca = byname(e, "mca"), m = gmem(64), out = gmem(256), desc = gmem(64);
    CHECK(mr && mc && m34 && c43 && ma && mca);
    CHECKEQ(vcall(e, FX_GetParameterDesc, 2, c43, desc), D3D_OK);
    CHECKEQ(MEM32(desc + PD_Class), 3), CHECKEQ(MEM32(desc + PD_Rows), 4), CHECKEQ(MEM32(desc + PD_Columns), 3);
    CHECKEQ(MEM32(desc + PD_Bytes), 48);
    /* defaults: mr's storage is 1..16 by rows, mc's 1..16 by columns */
    CHECKEQ(vcall(e, FX_GetMatrix, 2, mr, out), D3D_OK), mat_is(out, 1, 0, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, mc, out), D3D_OK), mat_is(out, 1, 1, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrixTranspose, 2, mc, out), D3D_OK), mat_is(out, 1, 0, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, m34, out), D3D_OK), mat_is(out, 1, 0, 3, 4);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, c43, out), D3D_OK);            /* storage: 3 columns of 4 */
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) CHECKF(gf(out + 4 * (4 * r + c)), c < 3 ? 1.0f + (float)(4 * c + r) : 0.0f);
    /* SetMatrix then Get, both ways */
    mat_from(m, 20);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, mr, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, mr, out), D3D_OK), mat_is(out, 20, 0, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrixTranspose, 2, mr, out), D3D_OK), mat_is(out, 20, 1, 4, 4);
    CHECKEQ(vcall(e, FX_GetValue, 3, mr, out, 64u), D3D_OK), mat_is(out, 20, 0, 4, 4);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, mc, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, mc, out), D3D_OK), mat_is(out, 20, 0, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrixTranspose, 2, mc, out), D3D_OK), mat_is(out, 20, 1, 4, 4);
    CHECKEQ(vcall(e, FX_GetValue, 3, mc, out, 64u), D3D_OK), mat_is(out, 20, 1, 4, 4);   /* stored by columns */
    CHECKEQ(vcall(e, FX_SetMatrixTranspose, 2, mr, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, mr, out), D3D_OK), mat_is(out, 20, 1, 4, 4);
    CHECKEQ(vcall(e, FX_SetMatrixTranspose, 2, mc, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetValue, 3, mc, out, 64u), D3D_OK), mat_is(out, 20, 0, 4, 4);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, m34, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, m34, out), D3D_OK), mat_is(out, 20, 0, 3, 4);
    CHECKEQ(vcall(e, FX_GetValue, 3, m34, out, 48u), D3D_OK);
    for (int k = 0; k < 12; k++) CHECKF(gf(out + 4 * k), 20.0f + (float)k);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, c43, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrix, 2, c43, out), D3D_OK), mat_is(out, 20, 0, 4, 3);
    CHECKEQ(vcall(e, FX_GetValue, 3, c43, out, 48u), D3D_OK);      /* column c, row r at 4c + r */
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 4; r++) CHECKF(gf(out + 4 * (4 * c + r)), 20.0f + (float)(4 * r + c));
    /* arrays, by value and by pointers */
    uint32_t two = gmem(128), ptrs = gmem(8);
    mat_from(two, 40), mat_from(two + 64, 60);
    MEM32(ptrs) = two + 64, MEM32(ptrs + 4) = two;
    CHECKEQ(vcall(e, FX_SetMatrixArray, 3, ma, two, 2u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrixArray, 3, ma, out, 2u), D3D_OK), mat_is(out, 40, 0, 4, 4), mat_is(out + 64, 60, 0, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrixTransposeArray, 3, ma, out, 2u), D3D_OK), mat_is(out, 40, 1, 4, 4);
    CHECKEQ(vcall(e, FX_SetMatrixPointerArray, 3, ma, ptrs, 2u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrixArray, 3, ma, out, 2u), D3D_OK), mat_is(out, 60, 0, 4, 4), mat_is(out + 64, 40, 0, 4, 4);
    uint32_t optrs = gmem(8);
    MEM32(optrs) = out + 128, MEM32(optrs + 4) = out;
    CHECKEQ(vcall(e, FX_GetMatrixPointerArray, 3, ma, optrs, 2u), D3D_OK);
    mat_is(out + 128, 60, 0, 4, 4), mat_is(out, 40, 0, 4, 4);
    CHECKEQ(vcall(e, FX_GetMatrixTransposePointerArray, 3, ma, optrs, 2u), D3D_OK);
    mat_is(out + 128, 60, 1, 4, 4), mat_is(out, 40, 1, 4, 4);
    CHECKEQ(vcall(e, FX_SetMatrixTransposeArray, 3, mca, two, 2u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrixArray, 3, mca, out, 2u), D3D_OK), mat_is(out, 40, 1, 4, 4), mat_is(out + 64, 60, 1, 4, 4);
    CHECKEQ(vcall(e, FX_GetValue, 3, mca, out, 128u), D3D_OK), mat_is(out, 40, 0, 4, 4);   /* by columns of M^T */
    CHECKEQ(vcall(e, FX_SetMatrixTransposePointerArray, 3, mca, ptrs, 2u), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrixTransposeArray, 3, mca, out, 2u), D3D_OK), mat_is(out, 60, 0, 4, 4);
    CHECKEQ(vcall(e, FX_SetMatrixArray, 3, ma, two, 3u), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, ma, m), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(e, FX_SetMatrixArray, 3, mr, two, 1u), D3DERR_INVALIDCALL);
    /* one element of an array */
    uint32_t e1 = vcall(e, FX_GetParameterElement, 2, ma, 1u);
    CHECKEQ(vcall(e, FX_SetMatrix, 2, e1, m), D3D_OK);
    CHECKEQ(vcall(e, FX_GetMatrixArray, 3, ma, out, 2u), D3D_OK), mat_is(out + 64, 20, 0, 4, 4);
    CHECKEQ(vcall(e, FX_Release, 0), 0);
    CHECKEQ(d3d_live_count(), live0);
}

/* ---- fx_pool: test_effects.py's pool_a (shared g and sv, own h), pool_b (own h, shared g), pool_bad ---- */

static void group_fx_pool(const char *a, const char *b, const char *bad) {
    uint32_t dev = new_device(), live0 = d3d_live_count(), po = gmem(4), out = gmem(16);
    CHECKEQ(call("D3DXCreateEffectPool", 1, po), D3D_OK);
    uint32_t pool = MEM32(po);
    CHECK(pool != 0);
    CHECKEQ(call("D3DXCreateEffectPool", 1, 0u), D3DERR_INVALIDCALL);
    uint32_t ea = create_file(dev, a, pool), eb = create_file(dev, b, pool), ec = create_file(dev, a, 0);
    if (!ea || !eb || !ec) return;
    uint32_t desc = gmem(64);
    CHECKEQ(vcall(ea, FX_GetParameterDesc, 2, byname(ea, "g"), desc), D3D_OK);
    CHECKEQ(MEM32(desc + PD_Flags), 1u);                           /* D3DX_PARAMETER_SHARED */
    /* b joined the pool after a: g has a's value, not b's own default; h is b's */
    CHECKEQ(vcall(eb, FX_GetFloat, 2, gstr("g"), out), D3D_OK), CHECKF(gf(out), 1.0f);
    CHECKEQ(vcall(eb, FX_GetFloat, 2, gstr("h"), out), D3D_OK), CHECKF(gf(out), 5.0f);
    struct fx_effect *fb = fx_effect_of(eb);
    struct fx_param *bg = fx_param_of(fb, gstr("g")), *bh = fx_param_of(fb, gstr("h"));
    CHECK(bg && bh);
    uint64_t sg = bg ? *bg->serial : 0, sh = bh ? *bh->serial : 0;
    /* set on a, seen by b; not by the effect outside the pool */
    CHECKEQ(vcall(ea, FX_SetFloat, 2, gstr("g"), f2u(4.0f)), D3D_OK);
    CHECKEQ(vcall(eb, FX_GetFloat, 2, gstr("g"), out), D3D_OK), CHECKF(gf(out), 4.0f);
    CHECKEQ(vcall(ec, FX_GetFloat, 2, gstr("g"), out), D3D_OK), CHECKF(gf(out), 1.0f);
    if (bg) CHECK(*bg->serial > sg);                               /* b's g is dirty: it re-uploads */
    CHECKEQ(vcall(ea, FX_SetFloat, 2, gstr("h"), f2u(6.0f)), D3D_OK);
    CHECKEQ(vcall(eb, FX_GetFloat, 2, gstr("h"), out), D3D_OK), CHECKF(gf(out), 5.0f);
    CHECKEQ(vcall(ea, FX_GetFloat, 2, gstr("h"), out), D3D_OK), CHECKF(gf(out), 6.0f);
    if (bh) CHECKEQ(*bh->serial, sh);                              /* b's h is not */
    CHECKEQ(vcall(eb, FX_SetFloat, 2, gstr("g"), f2u(7.0f)), D3D_OK);
    CHECKEQ(vcall(ea, FX_GetFloat, 2, gstr("g"), out), D3D_OK), CHECKF(gf(out), 7.0f);
    uint32_t v4 = gbytes((float[4]){ 5, 6, 7, 8 }, 16);
    CHECKEQ(vcall(ea, FX_SetVector, 2, gstr("sv"), v4), D3D_OK);
    CHECKEQ(vcall(ec, FX_GetVector, 2, gstr("sv"), out), D3D_OK), CHECKF(gf(out), 1.0f);
    /* GetPool */
    CHECKEQ(vcall(ea, FX_GetPool, 1, out), D3D_OK), CHECKEQ(MEM32(out), pool);
    CHECKEQ(refs(pool), 4);                                        /* ours, a's, b's and GetPool's */
    vcall(pool, 2, 0);
    MEM32(out) = 0xCDCDCDCDu;
    CHECKEQ(vcall(ec, FX_GetPool, 1, out), D3D_OK), CHECKEQ(MEM32(out), 0);
    /* a shared parameter of another type: D3DERR_INVALIDCALL */
    uint32_t n, hr;
    uint8_t *d = read_file(bad, &n);
    if (d) CHECKEQ(create(dev, d, n, pool, &hr), 0), CHECKEQ(hr, D3DERR_INVALIDCALL);
    if (d) CHECK(create(dev, d, n, 0, NULL) != 0);                 /* fine without the pool */
    free(d);
    /* a clone stays in the pool */
    CHECKEQ(vcall(eb, FX_CloneEffect, 2, dev, out), D3D_OK);
    uint32_t kb = MEM32(out);
    CHECKEQ(vcall(kb, FX_GetFloat, 2, gstr("h"), out), D3D_OK), CHECKF(gf(out), 5.0f);
    CHECKEQ(vcall(kb, FX_SetFloat, 2, gstr("g"), f2u(7.0f)), D3D_OK);
    CHECKEQ(vcall(kb, FX_GetPool, 1, out), D3D_OK), CHECKEQ(MEM32(out), pool);
    vcall(pool, 2, 0);
    CHECKEQ(vcall(kb, FX_Release, 0), 0);
    /* the pool keeps the value: a new effect sees it */
    CHECKEQ(vcall(ea, FX_Release, 0), 0);
    CHECKEQ(vcall(eb, FX_GetFloat, 2, gstr("g"), out), D3D_OK), CHECKF(gf(out), 7.0f);
    uint32_t ea2 = create_file(dev, a, pool);
    CHECKEQ(vcall(ea2, FX_GetFloat, 2, gstr("g"), out), D3D_OK), CHECKF(gf(out), 7.0f);
    CHECKEQ(vcall(ea2, FX_GetVector, 2, gstr("sv"), out), D3D_OK), CHECKF(gf(out + 12), 8.0f);
    CHECKEQ(vcall(ea2, FX_GetFloat, 2, gstr("h"), out), D3D_OK), CHECKF(gf(out), 2.0f);
    CHECKEQ(vcall(ea2, FX_Release, 0), 0);
    CHECKEQ(vcall(eb, FX_Release, 0), 0);
    CHECKEQ(vcall(ec, FX_Release, 0), 0);
    CHECKEQ(vcall(pool, 2, 0), 0);
    CHECKEQ(d3d_live_count(), live0 + 1);                          /* the effect made without the pool */
}

/* ---- fx_parse_errors: every truncation of a good binary fails cleanly; bad numbers fail ---- */

static void group_fx_parse_errors(const char *path) {
    uint32_t n;
    uint8_t *d = read_file(path, &n);
    if (!d) return;
    char err[256];
    struct fx_effect e;
    memset(&e, 0, sizeof e);
    CHECKEQ(fx_parse(d, n, &e, err, sizeof err), 0);
    fx_free(&e);
    for (uint32_t len = 0; len < n; len += 4) {
        memset(&e, 0, sizeof e);
        err[0] = 0;
        if (fx_parse(d, len, &e, err, sizeof err) == 0) { printf("FAIL truncated to %u parses\n", len); fails++; }
        else if (!err[0]) { printf("FAIL truncated to %u: no message\n", len); fails++; }
        fx_free(&e);
    }
    uint32_t magic = 0xFEFF0900u;
    uint8_t *c = malloc(n);
    memcpy(c, d, n), memcpy(c, &magic, 4);
    memset(&e, 0, sizeof e);
    CHECK(fx_parse(c, n, &e, err, sizeof err) != 0);
    fx_free(&e);
    printf("bad magic: %s\n", err);
    /* the first parameter's class (its typedef is at the start of the data): out of range */
    uint32_t body, toff;
    memcpy(&body, d + 4, 4), memcpy(&toff, d + 8 + body + 16, 4);
    memcpy(c, d, n);
    uint32_t bad = 9;
    memcpy(c + 8 + toff + 4, &bad, 4);
    memset(&e, 0, sizeof e);
    CHECK(fx_parse(c, n, &e, err, sizeof err) != 0);
    fx_free(&e);
    printf("bad class: %s\n", err);
    free(c);
    free(d);
}

/* ---- fx_text: HLSL source is plan 7's ---- */

static void group_fx_text(void) {
    uint32_t dev = new_device();
    static const char src[] = "float4 Tint;\ntechnique t { pass p { } }\n";
    create(dev, (const uint8_t *)src, sizeof src - 1, 0, NULL);
    printf("fx_text: returned\n");
}

/* ---- fx_game_parse: the game's effects (the fx_2_0 binaries, one per file in dir) ---- */

static uint32_t g_ops[256], g_sops[256];
static void count_params(struct fx_param *p, uint32_t n, uint32_t *shared) {
    for (uint32_t i = 0; i < n; i++) if (p[i].flags & FX_SHARED) (*shared)++;
}
static void sampler_ops(struct fx_param *leaf, void *ctx) {
    (void)ctx;
    for (uint32_t i = 0; i < leaf->nstates; i++) g_sops[leaf->states[i].op & 0xFF]++;
}

static void group_fx_game_parse(const char *dir) {
    DIR *dd = opendir(dir);
    if (!dd) { printf("FAIL cannot open %s\n", dir); fails++; return; }
    char **names = NULL;
    int nn = 0;
    for (struct dirent *de; (de = readdir(dd));)
        if (de->d_name[0] != '.') names = realloc(names, sizeof *names * (size_t)(nn + 1)), names[nn++] = strdup(de->d_name);
    closedir(dd);
    for (int i = 1; i < nn; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t; }
    uint32_t dev = new_device(), po = gmem(4), tp = 0, tt = 0, tpass = 0, tshared = 0, ok = 0, made = 0;
    CHECKEQ(call("D3DXCreateEffectPool", 1, po), D3D_OK);
    for (int i = 0; i < nn; i++) {
        char path[1024], err[256] = "";
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        uint32_t n;
        uint8_t *d = read_file(path, &n);
        if (!d) continue;
        struct fx_effect e;
        memset(&e, 0, sizeof e);
        if (fx_parse(d, n, &e, err, sizeof err)) { printf("FAIL %s: %s\n", names[i], err); fails++; free(d); continue; }
        uint32_t passes = 0, shared = 0;
        for (uint32_t t = 0; t < e.ntechs; t++) {
            passes += e.techs[t].npasses;
            for (uint32_t p = 0; p < e.techs[t].npasses; p++)
                for (uint32_t s = 0; s < e.techs[t].passes[p].nstates; s++) g_ops[e.techs[t].passes[p].states[s].op & 0xFF]++;
        }
        for (uint32_t k = 0; k < e.nparams; k++) fx_leaves(&e.params[k], sampler_ops, NULL);
        count_params(e.params, e.nparams, &shared);
        printf("%s: params %u techniques %u passes %u shared %u\n", names[i], e.nparams, e.ntechs, passes, shared);
        tp += e.nparams, tt += e.ntechs, tpass += passes, tshared += shared, ok++;
        fx_free(&e);
        uint32_t hr, fx = create(dev, d, n, MEM32(po), &hr);         /* all of them in one pool, as the game does */
        if (hr != D3D_OK) { printf("FAIL %s: D3DXCreateEffect 0x%08X\n", names[i], hr); fails++; }
        if (fx) made++;
        free(d);
    }
    printf("parsed %u created %u\n", ok, made);
    printf("totals: params %u techniques %u passes %u shared %u\n", tp, tt, tpass, tshared);
    printf("pass ops:");
    for (int i = 0; i < 256; i++) if (g_ops[i]) printf(" %d", i);
    printf("\nsampler ops:");
    for (int i = 0; i < 256; i++) if (g_sops[i]) printf(" %d", i);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fx_selftest <group> [args]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    const char *a1 = argc > 2 ? argv[2] : NULL, *a2 = argc > 3 ? argv[3] : NULL, *a3 = argc > 4 ? argv[4] : NULL;
    if (!strcmp(argv[1], "fx_params") && a1) group_fx_params(a1);
    else if (!strcmp(argv[1], "fx_matrix") && a1) group_fx_matrix(a1);
    else if (!strcmp(argv[1], "fx_pool") && a3) group_fx_pool(a1, a2, a3);
    else if (!strcmp(argv[1], "fx_parse_errors") && a1) group_fx_parse_errors(a1);
    else if (!strcmp(argv[1], "fx_text")) group_fx_text();
    else if (!strcmp(argv[1], "fx_game_parse") && a1) group_fx_game_parse(a1);
    else { fprintf(stderr, "fx_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
