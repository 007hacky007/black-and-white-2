/*
 * d3d9_selftest.c - the D3D9, DirectDraw and D3DX layers driven as lifted
 * code drives them: arguments pushed right to left, a return address, then
 * the shim; COM methods through the guest vtable. Each check also verifies
 * the stdcall pop. Built with the engine (minus main.c) by tests/test_d3d9.py;
 * `d3d9_selftest <group>` runs one group and prints "<group>: <n> failed".
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)

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

/* An import by name. */
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

/* A COM method: slot of obj's vtable, with this and n more arguments. */
static const char *g_last_method;
static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    g_last_method = g_cur_import;
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, g_cur_import, n + 1, a);
}
static const char *method_name(uint32_t obj, int slot) {
    recomp_lookup_import(MEM32(MEM32(obj) + 4u * (uint32_t)slot));
    return g_cur_import ? g_cur_import : "";
}

static uint32_t gmem(uint32_t n) { return hle_va(hle_alloc(n)); }
static uint32_t gstr(const char *s) { uint32_t p = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(p), s); return p; }

/* D3D9 numbers the tests use */
#define D3D_SDK_VERSION 32u
#define HAL 1u
#define REF 2u
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_D16_LOCKABLE 70u
#define FMT_D16 80u
#define FMT_D24S8 75u
#define FMT_DXT1 0x31545844u
#define RT_SURFACE 1u
#define RT_TEXTURE 3u
#define USAGE_DEPTHSTENCIL 2u

/* IDirect3D9 slots */
enum { D9_GetAdapterCount = 4, D9_GetAdapterIdentifier, D9_GetAdapterModeCount, D9_EnumAdapterModes,
       D9_GetAdapterDisplayMode, D9_CheckDeviceType, D9_CheckDeviceFormat, D9_CheckDeviceMultiSampleType,
       D9_CheckDepthStencilMatch, D9_CheckDeviceFormatConversion, D9_GetDeviceCaps, D9_GetAdapterMonitor,
       D9_CreateDevice };

static uint32_t new_d3d9(void) { return call("Direct3DCreate9", 1, D3D_SDK_VERSION); }

static void group_adapter(void) {
    uint32_t d = new_d3d9();
    CHECK(d && d != 0xDEADBEEFu);
    CHECK(!strcmp(method_name(d, 16), "IDirect3D9::CreateDevice"));
    CHECK(!strcmp(method_name(d, 4), "IDirect3D9::GetAdapterCount"));
    CHECKEQ(vcall(d, D9_GetAdapterCount, 0), 1);

    uint32_t id = gmem(1100);
    CHECKEQ(vcall(d, D9_GetAdapterIdentifier, 3, 0u, 0u, id), D3D_OK);
    CHECK(!strcmp((char *)hle_ptr(id), "bw2metal.dll"));
    CHECK(!strcmp((char *)hle_ptr(id + 512), "BW2 virtual GPU (Metal)"));
    CHECKEQ(MEM32(id + 1056), (10u << 16) | 0u);           /* DriverVersion low: 10.0 */
    CHECKEQ(MEM32(id + 1060), (6u << 16) | 14u);           /* high: 6.14 */
    CHECKEQ(MEM32(id + 1064), 0x106B);
    CHECKEQ(MEM32(id + 1068), 1);
    CHECKEQ(MEM32(id + 1080), 0xB2B2B2B2u);
    CHECKEQ(MEM32(id + 1096), 1);
    CHECKEQ(vcall(d, D9_GetAdapterIdentifier, 3, 1u, 0u, id), D3DERR_INVALIDCALL);

    uint32_t n = vcall(d, D9_GetAdapterModeCount, 2, 0u, FMT_X8R8G8B8);
    CHECK(n >= 10);
    CHECKEQ(vcall(d, D9_GetAdapterModeCount, 2, 0u, FMT_R5G6B5), n);
    CHECKEQ(vcall(d, D9_GetAdapterModeCount, 2, 0u, FMT_A8R8G8B8), 0);
    uint32_t m = gmem(16), pw = 0, ph = 0, saw1080 = 0, saw1200 = 0;
    for (uint32_t i = 0; i < n; i++) {
        CHECKEQ(vcall(d, D9_EnumAdapterModes, 4, 0u, FMT_X8R8G8B8, i, m), D3D_OK);
        uint32_t w = MEM32(m), h = MEM32(m + 4);
        CHECK(w > pw || (w == pw && h > ph));                /* sorted, no duplicates */
        CHECKEQ(MEM32(m + 8), 60);
        CHECKEQ(MEM32(m + 12), FMT_X8R8G8B8);
        saw1080 |= w == 1920 && h == 1080;
        saw1200 |= w == 1920 && h == 1200;
        pw = w, ph = h;
    }
    CHECK(saw1080 && !saw1200);
    CHECK(pw == 1920 && ph == 1080);                         /* the headless display is the largest */
    CHECKEQ(vcall(d, D9_EnumAdapterModes, 4, 0u, FMT_X8R8G8B8, n, m), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(d, D9_GetAdapterDisplayMode, 2, 0u, m), D3D_OK);
    CHECK(MEM32(m) == 1920 && MEM32(m + 4) == 1080 && MEM32(m + 8) == 60 && MEM32(m + 12) == FMT_X8R8G8B8);

    CHECKEQ(vcall(d, D9_CheckDeviceType, 5, 0u, HAL, FMT_X8R8G8B8, FMT_A8R8G8B8, 1u), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceType, 5, 0u, HAL, FMT_X8R8G8B8, FMT_X8R8G8B8, 0u), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceType, 5, 0u, REF, FMT_X8R8G8B8, FMT_X8R8G8B8, 1u), D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, 0u, RT_TEXTURE, FMT_DXT1), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, USAGE_DEPTHSTENCIL, RT_SURFACE, FMT_D16_LOCKABLE),
            D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, USAGE_DEPTHSTENCIL, RT_SURFACE, FMT_D24S8), D3D_OK);
    uint32_t q = gmem(4);
    CHECKEQ(vcall(d, D9_CheckDeviceMultiSampleType, 6, 0u, HAL, FMT_X8R8G8B8, 1u, 0u, q), D3D_OK);
    CHECKEQ(MEM32(q), 1);
    CHECKEQ(vcall(d, D9_CheckDeviceMultiSampleType, 6, 0u, HAL, FMT_X8R8G8B8, 1u, 4u, q), D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_CheckDepthStencilMatch, 5, 0u, HAL, FMT_X8R8G8B8, FMT_A8R8G8B8, FMT_D24S8), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceFormatConversion, 4, 0u, HAL, FMT_A8R8G8B8, FMT_X8R8G8B8), D3D_OK);

    uint32_t caps = gmem(304);
    CHECKEQ(vcall(d, D9_GetDeviceCaps, 3, 0u, HAL, caps), D3D_OK);
    CHECKEQ(MEM32(caps + 0x00), HAL);
    CHECKEQ(MEM32(caps + 0xC4), 0xFFFE0300u);
    CHECKEQ(MEM32(caps + 0xCC), 0xFFFF0300u);
    CHECKEQ(MEM32(caps + 0xC8), 256);
    CHECKEQ(MEM32(caps + 0x58), 4096);
    CHECKEQ(MEM32(caps + 0x94), 8);
    CHECKEQ(MEM32(caps + 0x98), 8);
    CHECKEQ(MEM32(caps + 0xA0), 8);
    CHECKEQ(MEM32(caps + 0xF0), 4);
    CHECKEQ(MEM32(caps + 0x3C) & 0x102, 0);                  /* neither POW2 nor NONPOW2CONDITIONAL */
    CHECKEQ(vcall(d, D9_GetDeviceCaps, 3, 0u, REF, caps), D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_GetAdapterMonitor, 1, 0u), 0x4D4F4E31u);
    CHECKEQ(vcall(d, 3, 1, 0u), D3DERR_INVALIDCALL);         /* RegisterSoftwareDevice */
    CHECKEQ(vcall(d, 2, 0), 0);                              /* Release */
}

/* DirectDrawEnumerateA's callback: (guid, description, name, context) -> TRUE */
static int g_enum_calls;
static uint32_t g_enum_guid, g_enum_ctx;
static char g_enum_desc[64], g_enum_name[64];
static void enum_cb(void) {
    g_enum_calls++;
    g_enum_guid = A32(0), g_enum_ctx = A32(3);
    snprintf(g_enum_desc, sizeof g_enum_desc, "%s", (char *)APTR(1));
    snprintf(g_enum_name, sizeof g_enum_name, "%s", (char *)APTR(2));
    RET(1, 4);
}
static const win32hle_shim g_test_shims[] = { { "selftest_ddenum_cb", enum_cb }, { 0, 0 } };

static const uint8_t CLSID_DirectDraw[16] = { 0xE0, 0x0E, 0xB7, 0xD7, 0x40, 0x43, 0xCF, 0x11,
                                              0xB0, 0x63, 0x00, 0x20, 0xAF, 0xC2, 0xCD, 0x35 };
static const uint8_t IID_IDirectDraw7[16] = { 0xC0, 0x5E, 0xE6, 0x15, 0x9C, 0x3B, 0xD2, 0x11,
                                              0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B };

static void group_ddraw(void) {
    uint32_t clsid = gmem(16), iid = gmem(16), out = gmem(4);
    memcpy(hle_ptr(clsid), CLSID_DirectDraw, 16);
    memcpy(hle_ptr(iid), IID_IDirectDraw7, 16);
    CHECKEQ(call("CoInitialize", 1, 0u), 0);
    CHECKEQ(call("CoCreateInstance", 5, clsid, 0u, 7u, iid, out), 0);
    uint32_t dd = MEM32(out);
    CHECK(dd != 0);
    CHECK(!strcmp(method_name(dd, 23), "IDirectDraw7::GetAvailableVidMem"));
    CHECKEQ(vcall(dd, 18, 1, 0u), 0);                        /* Initialize(NULL) */
    uint32_t caps = gmem(16), total = gmem(4), freem = gmem(4);
    MEM32(caps) = 0x10000000u;                               /* DDSCAPS_LOCALVIDMEM */
    CHECKEQ(vcall(dd, 23, 3, caps, total, freem), 0);
    CHECKEQ(MEM32(total), 512u << 20);
    CHECKEQ(MEM32(freem), 512u << 20);
    CHECKEQ(vcall(dd, 2, 0), 0);

    CHECKEQ(call("DirectDrawCreateEx", 4, 0u, out, iid, 0u), 0);
    dd = MEM32(out);
    uint32_t id = gmem(1068);
    CHECKEQ(vcall(dd, 27, 2, id, 0u), 0);                    /* GetDeviceIdentifier */
    CHECK(!strcmp((char *)hle_ptr(id), "bw2metal.dll"));
    CHECK(!strcmp((char *)hle_ptr(id + 512), "BW2 virtual GPU (Metal)"));
    CHECKEQ(MEM32(id + 1024), (10u << 16) | 0u);
    CHECKEQ(MEM32(id + 1028), (6u << 16) | 14u);
    CHECKEQ(MEM32(id + 1032), 0x106B);
    CHECKEQ(MEM32(id + 1036), 1);
    CHECKEQ(MEM32(id + 1064), 1);
    CHECKEQ(vcall(dd, 2, 0), 0);

    win32hle_register(g_test_shims);
    CHECKEQ(call("DirectDrawEnumerateA", 2, hle_resolve("selftest_ddenum_cb"), 0x1234u), 0);
    CHECKEQ(g_enum_calls, 1);
    CHECKEQ(g_enum_guid, 0);
    CHECKEQ(g_enum_ctx, 0x1234);
    CHECK(!strcmp(g_enum_desc, "Primary Display Driver"));
    CHECK(!strcmp(g_enum_name, "display"));
}

struct test_obj { struct d3d_obj base; uint32_t n; };
static void group_objects(void) {
    enum { N = 200000 };
    uint32_t *objs = malloc(sizeof *objs * N);
    struct test_obj *hosts = calloc(N, sizeof *hosts);
    uint32_t live0 = d3d_live_count();
    for (uint32_t i = 0; i < N; i++) {
        hosts[i].n = i;
        objs[i] = d3d_new(D3DK_D3DXBUFFER, "ID3DXBuffer", &hosts[i].base);
    }
    for (uint32_t i = 0; i < N; i += 2) CHECKEQ(d3d_release(objs[i]), 0);
    CHECKEQ(d3d_live_count() - live0, N / 2);
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (i % 2) bad += ((struct test_obj *)d3d_host(objs[i], D3DK_D3DXBUFFER))->n != i;
        else bad += d3d_alive(objs[i]);
    }
    CHECKEQ(bad, 0);
    /* freed ids are reused, and an old handle never resolves to a new object */
    struct test_obj again = { .n = 7 };
    uint32_t o = d3d_new(D3DK_D3DXBUFFER, "ID3DXBuffer", &again.base);
    CHECK(d3d_alive(o));
    CHECKEQ(vcall(o, 1, 0), 2);                              /* AddRef through the vtable */
    CHECKEQ(vcall(o, 2, 0), 1);
}

static void group_unimplemented(void) {
    struct d3d_obj dev = { 0 };
    uint32_t o = d3d_new(D3DK_DEVICE, "IDirect3DDevice9", &dev);
    vcall(o, 115, 5, 0u, 0u, 0u, 0u, 0u);                    /* DrawRectPatch: never implemented */
    printf("unimplemented: returned\n");
}

static const struct { const char *name; void (*fn)(void); } g_groups[] = {
    { "adapter", group_adapter }, { "ddraw", group_ddraw }, { "objects", group_objects },
    { "unimplemented", group_unimplemented },
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: d3d9_selftest <group>\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    for (size_t i = 0; i < sizeof g_groups / sizeof *g_groups; i++)
        if (!strcmp(argv[1], g_groups[i].name)) {
            g_groups[i].fn();
            printf("%s: %d failed\n", argv[1], fails);
            fflush(stdout);
            return fails != 0;
        }
    fprintf(stderr, "d3d9_selftest: no group %s\n", argv[1]);
    return 2;
}
