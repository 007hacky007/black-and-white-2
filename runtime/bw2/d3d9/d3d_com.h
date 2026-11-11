/*
 * d3d_com.h - guest COM objects for the D3D9, DirectDraw 7 and D3DX layers
 * (plan 4). A guest object is [vtable][reference count][host id]; the id
 * indexes a growable table of host structs (no fixed-size tables, spec 7.2).
 * Each method is a stdcall shim whose argument 0 is `this`.
 */
#ifndef D3D_COM_H
#define D3D_COM_H
#include <stdint.h>
#include "win32hle.h"

enum d3d_kind {
    D3DK_NONE, D3DK_D3D9, D3DK_DEVICE, D3DK_SWAPCHAIN, D3DK_TEXTURE, D3DK_CUBETEXTURE, D3DK_VOLUMETEXTURE,
    D3DK_SURFACE, D3DK_VOLUME, D3DK_VB, D3DK_IB, D3DK_VDECL, D3DK_VSHADER, D3DK_PSHADER, D3DK_STATEBLOCK,
    D3DK_QUERY, D3DK_DDRAW7, D3DK_D3DXBUFFER, D3DK_COUNT
};

/* Every host struct starts with this. destroy (may be NULL) runs at the last Release. */
struct d3d_obj {
    enum d3d_kind kind;
    uint32_t guest;                         /* the guest object */
    void (*destroy)(struct d3d_obj *self);
};

struct d3d_iface {
    const char *name;                       /* "IDirect3DDevice9" */
    const char *const *methods;             /* vtable order, NULL-terminated */
    uint8_t iids[4][16];                    /* own IID, then bases'; all-zero ends */
};
extern const struct d3d_iface d3d_ifaces[];

/* HRESULTs */
#define D3D_OK                  0u
#define S_FALSE_                1u
#define E_NOINTERFACE_          0x80004002u
#define E_OUTOFMEMORY_          0x8007000Eu
#define D3DERR_INVALIDCALL      0x8876086Cu
#define D3DERR_NOTAVAILABLE     0x8876086Au
#define D3DERR_NOTFOUND         0x88760866u
#define D3DERR_MOREDATA         0x88760867u
#define D3DERR_DEVICELOST       0x88760868u
#define D3DERR_WASSTILLDRAWING  0x8876021Cu
#define D3DOK_NOAUTOGEN         0x0876086Fu

/* Register an implementation ("IDirect3D9::GetAdapterCount") before vtables are built. */
void d3d_impl(const char *iface_method, recomp_func_t fn);
void d3d_impls(const win32hle_shim *table);          /* a { name, fn } table ending { 0, 0 } */
/* The guest vtable for an interface, built once (unimplemented methods stop loudly). */
uint32_t d3d_vtable(const char *iface);

/* A new guest object of kind with host struct host (host->kind/guest are set here). */
uint32_t d3d_new(enum d3d_kind kind, const char *iface, struct d3d_obj *host);
/* The host struct of obj; stops loudly on a wrong kind or a dead object. */
void *d3d_host(uint32_t obj, enum d3d_kind kind);
void *d3d_host_any(uint32_t obj);                    /* any live object, or NULL */
int d3d_alive(uint32_t obj);
/* The last Release: destroy, then free the id slot and the guest object. */
void d3d_release_host(uint32_t obj);
uint32_t d3d_addref(uint32_t obj);                   /* +1, the new count */
uint32_t d3d_release(uint32_t obj);                  /* -1, destroying at 0; the new count */
uint32_t d3d_live_count(void);                       /* live objects (tests) */

/* The IUnknown methods every interface shares. */
void d3d_QueryInterface(void);
void d3d_AddRef(void);
void d3d_Release(void);

/* Stop the run: "d3d9: <what> (from guest 0x%08X)". */
_Noreturn void d3d_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Exit 4: a method plan 5 owns. */
_Noreturn void d3d_later(const char *what);

#endif
