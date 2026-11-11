/*
 * d3d_com.c - the object table and vtables of d3d_com.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d_com.h"
#include "bw2.h"

/* ---- implementations, by "Interface::Method" ---- */
static struct { const char *name; recomp_func_t fn; } *g_impl;
static int g_nimpl, g_capimpl;

void d3d_impl(const char *name, recomp_func_t fn) {
    for (int i = 0; i < g_nimpl; i++)
        if (!strcmp(g_impl[i].name, name)) { g_impl[i].fn = fn; return; }
    if (g_nimpl == g_capimpl) {
        g_capimpl = g_capimpl ? 2 * g_capimpl : 256;
        g_impl = realloc(g_impl, sizeof *g_impl * (size_t)g_capimpl);
        if (!g_impl) abort();
    }
    g_impl[g_nimpl].name = name, g_impl[g_nimpl++].fn = fn;
}
void d3d_impls(const win32hle_shim *t) {
    for (; t->name; t++) d3d_impl(t->name, t->fn);
}
static recomp_func_t impl_of(const char *name) {
    for (int i = 0; i < g_nimpl; i++)
        if (!strcmp(g_impl[i].name, name)) return g_impl[i].fn;
    return NULL;
}

_Noreturn void d3d_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bw2] FATAL: d3d9: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (from guest 0x%08X, sub_%08X)\n", MEM32(g_esp), g_cur_func);
    fflush(stderr);
    abort();
}
_Noreturn void d3d_later(const char *what) {
    fprintf(stderr, "[bw2] reached %s: owned by a later plan (from guest 0x%08X, sub_%08X)\n",
            what, MEM32(g_esp), g_cur_func);
    fflush(stderr);
    hle_exit(BW2_EXIT_LATER);
    abort();
}
static void unimplemented(void) { d3d_fail("%s not implemented", g_cur_import ? g_cur_import : "?"); }

/* ---- vtables ---- */
static struct { const char *iface; uint32_t vt; } g_vt[64];
static int g_nvt;

static const struct d3d_iface *iface_of(const char *name) {
    for (const struct d3d_iface *f = d3d_ifaces; f->name; f++)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}

uint32_t d3d_vtable(const char *iface) {
    for (int i = 0; i < g_nvt; i++)
        if (!strcmp(g_vt[i].iface, iface)) return g_vt[i].vt;
    const struct d3d_iface *f = iface_of(iface);
    if (!f) d3d_fail("no interface %s", iface);
    int n = 0;
    while (f->methods[n]) n++;
    win32hle_shim *t = calloc((size_t)n + 1, sizeof *t);
    for (int i = 0; i < n; i++) {
        char *full = malloc(strlen(iface) + strlen(f->methods[i]) + 3);
        sprintf(full, "%s::%s", iface, f->methods[i]);
        recomp_func_t fn = impl_of(full);
        if (!fn && i == 0) fn = d3d_QueryInterface;
        if (!fn && i == 1) fn = d3d_AddRef;
        if (!fn && i == 2) fn = d3d_Release;
        t[i].name = full, t[i].fn = fn ? fn : unimplemented;
    }
    uint32_t vt = hle_com_vtable(t);         /* registers the shims; keeps the table */
    if (g_nvt == (int)(sizeof g_vt / sizeof g_vt[0])) d3d_fail("too many interfaces");
    g_vt[g_nvt].iface = f->name, g_vt[g_nvt++].vt = vt;
    return vt;
}

/* ---- objects ---- */
static struct slot { struct d3d_obj *host; uint32_t guest; const struct d3d_iface *iface; } *g_slot;
static uint32_t g_nslot, g_capslot, *g_freeids, g_nfree, g_capfree, g_live;

uint32_t d3d_new(enum d3d_kind kind, const char *iface, struct d3d_obj *host) {
    uint32_t vt = d3d_vtable(iface), id;
    if (g_nfree) id = g_freeids[--g_nfree];
    else {
        if (g_nslot == g_capslot) {
            g_capslot = g_capslot ? 2 * g_capslot : 1024;
            g_slot = realloc(g_slot, sizeof *g_slot * g_capslot);
            if (!g_slot) abort();
        }
        id = g_nslot++;
    }
    uint32_t obj = hle_com_new(vt, 12);
    MEM32(obj + 8) = id;
    host->kind = kind, host->guest = obj;
    g_slot[id].host = host, g_slot[id].guest = obj, g_slot[id].iface = iface_of(iface);
    g_live++;
    return obj;
}

static struct slot *slot_of(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t id = MEM32(obj + 8);
    if (id >= g_nslot || g_slot[id].guest != obj || !g_slot[id].host) return NULL;
    return &g_slot[id];
}
int d3d_alive(uint32_t obj) { return slot_of(obj) != NULL; }
void *d3d_host_any(uint32_t obj) { struct slot *s = slot_of(obj); return s ? s->host : NULL; }
void *d3d_host(uint32_t obj, enum d3d_kind kind) {
    struct slot *s = slot_of(obj);
    if (!s) d3d_fail("0x%08X is not a live object", obj);
    if (s->host->kind != kind) d3d_fail("0x%08X is a %s, not kind %d", obj, s->iface->name, kind);
    return s->host;
}

void d3d_release_host(uint32_t obj) {
    struct slot *s = slot_of(obj);
    if (!s) d3d_fail("release of dead object 0x%08X", obj);
    struct d3d_obj *h = s->host;
    uint32_t id = MEM32(obj + 8);
    s->host = NULL, s->guest = 0;
    if (g_nfree == g_capfree) {
        g_capfree = g_capfree ? 2 * g_capfree : 1024;
        g_freeids = realloc(g_freeids, sizeof *g_freeids * g_capfree);
        if (!g_freeids) abort();
    }
    g_freeids[g_nfree++] = id;
    g_live--;
    if (h->destroy) h->destroy(h);
    MEM32(obj) = 0, MEM32(obj + 8) = 0xFFFFFFFFu;  /* a stale call through it faults loudly */
    hle_com_free(obj);
}
uint32_t d3d_live_count(void) { return g_live; }

uint32_t d3d_addref(uint32_t obj) {
    if (!slot_of(obj)) d3d_fail("AddRef of dead object 0x%08X", obj);
    return ++MEM32(obj + 4);
}
uint32_t d3d_release(uint32_t obj) {
    if (!slot_of(obj)) d3d_fail("Release of dead object 0x%08X", obj);
    uint32_t n = --MEM32(obj + 4);
    if (!n) d3d_release_host(obj);
    return n;
}

static const uint8_t IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t zero16[16];

uint32_t d3d_qi(uint32_t self, const uint8_t *iid, uint32_t out) {
    struct slot *s = slot_of(self);
    if (!s) d3d_fail("QueryInterface on dead object 0x%08X", self);
    int ok = iid && !memcmp(iid, IID_IUnknown, 16);
    for (int i = 0; iid && !ok && i < 4 && memcmp(s->iface->iids[i], zero16, 16); i++)
        ok = !memcmp(iid, s->iface->iids[i], 16);
    if (!ok) {
        if (out) MEM32(out) = 0;
        return E_NOINTERFACE_;
    }
    d3d_addref(self);
    if (out) MEM32(out) = self;
    return D3D_OK;
}
void d3d_QueryInterface(void) { RET(d3d_qi(A32(0), APTR(1), A32(2)), 3); }   /* (this, riid, ppv) */
void d3d_AddRef(void) { RET(d3d_addref(A32(0)), 1); }
void d3d_Release(void) { RET(d3d_release(A32(0)), 1); }
