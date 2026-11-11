/*
 * d3d9_surface.c - IDirect3DSurface9 (plan 4 Tasks 4-5): render targets,
 * depth surfaces, offscreen plain surfaces and the levels of textures. A lock
 * returns a guest-memory copy in D3D's own layout; the unlock uploads it.
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

#define D3DRTYPE_SURFACE_ 1u

struct d3d_surface *d3d_surf(uint32_t obj) { return d3d_host(obj, D3DK_SURFACE); }

static void destroy_surface(struct d3d_obj *o) {
    struct d3d_surface *s = (struct d3d_surface *)o;
    if (s->mem) hle_free(hle_ptr(s->mem));
    if (s->owns_tex) mtl_release(s->tex);
    if (s->device && d3d_alive(s->device)) {
        struct d3d_device *d = d3d_host(s->device, D3DK_DEVICE);
        if (s->counts_for_reset) d->default_pool_live--;
    }
    if (s->container) d3d_release(s->container);
    else if (s->device && s->counts_for_reset) d3d_release(s->device);
    free(s);
}

uint32_t d3d_surface_new(uint32_t device, uint32_t container, uint32_t fmt, uint32_t usage, uint32_t pool,
                         uint32_t w, uint32_t h, mtl_obj tex, uint32_t slice, uint32_t level, int counts_for_reset) {
    struct d3d_surface *s = calloc(1, sizeof *s);
    s->base.destroy = destroy_surface;
    s->device = device, s->container = container;
    s->fmt = fmt, s->type_usage = usage, s->pool = pool, s->w = w, s->h = h;
    s->f = d3d_format(fmt);
    s->slice = slice, s->level = level;
    s->pitch = d3d_format_pitch(s->f, w);
    s->mem_size = s->pitch * d3d_format_rows(s->f, h);
    struct d3d_device *d = d3d_host(device, D3DK_DEVICE);
    if (!tex && (pool == D3DPOOL_DEFAULT || pool == D3DPOOL_MANAGED)) {
        uint32_t flags = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ? MT_RENDER : 0;
        tex = mtl_tex_new(d->mtl, s->f->store, MT_2D, w, h, 1, 1, flags);
        s->owns_tex = 1;
    }
    s->tex = tex;
    s->counts_for_reset = counts_for_reset;
    if (counts_for_reset) {
        d->default_pool_live++;
        if (!container) d3d_addref(device);
    }
    if (container) d3d_addref(container);
    return d3d_new(D3DK_SURFACE, "IDirect3DSurface9", &s->base);
}

uint32_t d3d_surface_mem(struct d3d_surface *s) {
    if (!s->mem) s->mem = hle_va(hle_alloc(s->mem_size ? s->mem_size : 16));
    return s->mem;
}

void d3d_surface_upload(struct d3d_surface *s, const struct d3d_rect *r) {
    if (!s->tex) return;
    struct d3d_device *d = d3d_host(s->device, D3DK_DEVICE);
    uint32_t bs = s->f->block, x0 = (uint32_t)r->l / bs * bs, y0 = (uint32_t)r->t / bs * bs;
    uint32_t x1 = (uint32_t)r->r, y1 = (uint32_t)r->b;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t w = x1 - x0, h = y1 - y0, rows = d3d_format_rows(s->f, h);
    const uint8_t *src = (const uint8_t *)hle_ptr(d3d_surface_mem(s)) + (y0 / bs) * s->pitch +
                         (x0 / bs) * s->f->bytes;
    if (s->f->conv == CV_NONE) {
        mtl_tex_upload(d->mtl, s->tex, s->slice, s->level, x0, y0, 0, w, h, 1, src, s->pitch, s->pitch * rows);
        return;
    }
    uint32_t sp = d3d_storage_pitch(s->f, w);
    uint8_t *tmp = malloc((size_t)sp * rows);
    d3d_to_storage(s->f, src, s->pitch, tmp, sp, w, rows);
    mtl_tex_upload(d->mtl, s->tex, s->slice, s->level, x0, y0, 0, w, h, 1, tmp, sp, sp * rows);
    free(tmp);
}

void d3d_surface_download(struct d3d_surface *s) {
    if (!s->tex) return;
    struct d3d_device *d = d3d_host(s->device, D3DK_DEVICE);
    uint32_t sp = d3d_storage_pitch(s->f, s->w), rows = d3d_format_rows(s->f, s->h);
    uint8_t *tmp = malloc((size_t)sp * rows);
    mtl_tex_download(d->mtl, s->tex, s->slice, s->level, 0, 0, s->w, s->h, tmp, sp);
    d3d_from_storage(s->f, tmp, sp, hle_ptr(d3d_surface_mem(s)), s->pitch, s->w, rows);
    free(tmp);
}

/* ---- methods ---- */
static void sf_GetDevice(void) {                       /* (this, IDirect3DDevice9**) */
    struct d3d_surface *s = d3d_surf(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(s->device);
    MEM32(A32(1)) = s->device;
    RET(D3D_OK, 2);
}
static void sf_GetType(void) { (void)d3d_surf(A32(0)); RET(D3DRTYPE_SURFACE_, 1); }
static void sf_GetContainer(void) {                    /* (this, riid, void**) */
    struct d3d_surface *s = d3d_surf(A32(0));
    uint32_t obj = s->container ? s->container : s->device;
    RET(d3d_qi(obj, APTR(1), A32(2)), 3);
}
static void sf_GetDesc(void) {                         /* (this, D3DSURFACE_DESC*) */
    struct d3d_surface *s = d3d_surf(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(D3DERR_INVALIDCALL, 2);
    MEM32(p) = s->fmt, MEM32(p + 4) = D3DRTYPE_SURFACE_, MEM32(p + 8) = s->type_usage, MEM32(p + 12) = s->pool;
    MEM32(p + 16) = s->ms, MEM32(p + 20) = s->msq, MEM32(p + 24) = s->w, MEM32(p + 28) = s->h;
    RET(D3D_OK, 2);
}
static void sf_LockRect(void) {                        /* (this, D3DLOCKED_RECT*, RECT*, flags) */
    struct d3d_surface *s = d3d_surf(A32(0));
    uint32_t lr = A32(1), flags = A32(3);
    if (!lr || s->locked) RET(D3DERR_INVALIDCALL, 4);
    struct d3d_rect r = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    if (A32(2)) memcpy(&r, APTR(2), sizeof r);
    if (r.l < 0 || r.t < 0 || r.r > (int32_t)s->w || r.b > (int32_t)s->h || r.l >= r.r || r.t >= r.b)
        RET(D3DERR_INVALIDCALL, 4);
    if (s->f->block == 4 && ((r.l | r.t) & 3)) RET(D3DERR_INVALIDCALL, 4);
    if (s->tex && (s->type_usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && !(flags & D3DLOCK_DISCARD))
        d3d_surface_download(s);                         /* a target's contents are on the GPU */
    uint32_t bs = s->f->block;
    MEM32(lr) = s->pitch;
    MEM32(lr + 4) = d3d_surface_mem(s) + ((uint32_t)r.t / bs) * s->pitch + ((uint32_t)r.l / bs) * s->f->bytes;
    s->locked = 1, s->lock_ro = (flags & D3DLOCK_READONLY) != 0, s->lock = r;
    RET(D3D_OK, 4);
}
static void sf_UnlockRect(void) {
    struct d3d_surface *s = d3d_surf(A32(0));
    if (!s->locked) RET(D3DERR_INVALIDCALL, 1);
    s->locked = 0;
    if (!s->lock_ro) d3d_surface_upload(s, &s->lock);
    RET(D3D_OK, 1);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3DSurface9::GetDevice", sf_GetDevice },
    { "IDirect3DSurface9::GetType", sf_GetType },
    { "IDirect3DSurface9::GetContainer", sf_GetContainer },
    { "IDirect3DSurface9::GetDesc", sf_GetDesc },
    { "IDirect3DSurface9::LockRect", sf_LockRect },
    { "IDirect3DSurface9::UnlockRect", sf_UnlockRect },
    { 0, 0 } };
void d3d9_surface_register(void) { d3d_impls(g_impls); }
