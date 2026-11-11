/*
 * d3d9_texture.c - IDirect3DTexture9, IDirect3DCubeTexture9,
 * IDirect3DVolumeTexture9 and IDirect3DVolume9, and the device's texture
 * bindings (plan 4 Task 5). A lock returns the level's guest staging copy in
 * D3D's layout; the unlock uploads the locked part in command order.
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

#define D3DRTYPE_VOLUME_ 2u
#define D3DRTYPE_TEXTURE_ 3u
#define D3DRTYPE_VOLUMETEXTURE_ 4u
#define D3DRTYPE_CUBETEXTURE_ 5u

static int is_tex_kind(enum d3d_kind k) { return k == D3DK_TEXTURE || k == D3DK_CUBETEXTURE || k == D3DK_VOLUMETEXTURE; }
struct d3d_texture *d3d_tex(uint32_t obj) {
    struct d3d_obj *o = d3d_host_any(obj);
    if (!o || !is_tex_kind(o->kind)) d3d_fail("0x%08X is not a live texture", obj);
    return (struct d3d_texture *)o;
}
static struct d3d_texture *tex_of_kind(uint32_t obj, enum d3d_kind k) { return d3d_host(obj, k); }

static uint32_t dim(uint32_t v, uint32_t l) { return v >> l ? v >> l : 1; }
uint32_t d3d_tex_level_w(const struct d3d_texture *t, uint32_t l) { return dim(t->w, l); }
uint32_t d3d_tex_level_h(const struct d3d_texture *t, uint32_t l) { return dim(t->h, l); }
uint32_t d3d_tex_level_d(const struct d3d_texture *t, uint32_t l) { return dim(t->d, l); }

static uint32_t level_pitch(const struct d3d_texture *t, uint32_t l) { return d3d_format_pitch(t->f, dim(t->w, l)); }
static uint32_t level_rows(const struct d3d_texture *t, uint32_t l) { return d3d_format_rows(t->f, dim(t->h, l)); }

uint32_t d3d_tex_mem(struct d3d_texture *t, uint32_t sub) {
    if (!t->mem[sub]) {
        uint32_t l = sub % t->levels, size = level_pitch(t, l) * level_rows(t, l) * dim(t->d, l);
        t->mem[sub] = hle_va(hle_alloc(size ? size : 16));
    }
    return t->mem[sub];
}

void d3d_tex_upload(struct d3d_texture *t, uint32_t sub, const struct d3d_box *b) {
    if (!t->tex) return;
    struct d3d_device *d = d3d_host(t->device, D3DK_DEVICE);
    uint32_t l = sub % t->levels, face = sub / t->levels, lw = dim(t->w, l), lh = dim(t->h, l), ld = dim(t->d, l);
    uint32_t bs = t->f->block, x0 = b->l / bs * bs, y0 = b->t / bs * bs, x1 = b->r < lw ? b->r : lw;
    uint32_t y1 = b->b < lh ? b->b : lh, z0 = b->front, z1 = b->back < ld ? b->back : ld;
    if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
    uint32_t pitch = level_pitch(t, l), rows_all = level_rows(t, l), slice = pitch * rows_all;
    uint32_t w = x1 - x0, h = y1 - y0, rows = d3d_format_rows(t->f, h);
    const uint8_t *base = (const uint8_t *)hle_ptr(d3d_tex_mem(t, sub)) + z0 * slice + (y0 / bs) * pitch +
                          (x0 / bs) * t->f->bytes;
    if (t->f->conv == CV_NONE) {
        mtl_tex_upload(d->mtl, t->tex, face, l, x0, y0, z0, w, h, z1 - z0, base, pitch, slice);
    } else {
        uint32_t sp = d3d_storage_pitch(t->f, w);
        uint8_t *tmp = malloc((size_t)sp * rows * (z1 - z0));
        for (uint32_t z = 0; z < z1 - z0; z++)
            d3d_to_storage(t->f, base + z * slice, pitch, tmp + (size_t)z * sp * rows, sp, w, rows);
        mtl_tex_upload(d->mtl, t->tex, face, l, x0, y0, z0, w, h, z1 - z0, tmp, sp, sp * rows);
        free(tmp);
    }
    if (t->autogen && l == 0) mtl_tex_mipgen(d->mtl, t->tex);
}

static void destroy_texture(struct d3d_obj *o) {
    struct d3d_texture *t = (struct d3d_texture *)o;
    uint32_t n = t->faces * t->levels;
    for (uint32_t i = 0; i < n; i++) if (t->mem[i]) hle_free(hle_ptr(t->mem[i]));
    mtl_release(t->view);
    mtl_release(t->srgb_view);
    mtl_release(t->tex);
    if (d3d_alive(t->device)) {
        struct d3d_device *d = d3d_host(t->device, D3DK_DEVICE);
        if (t->counts_for_reset) d->default_pool_live--;
    }
    free(t->mem), free(t->child), free(t->locked), free(t->lock_ro), free(t->lockbox);
    uint32_t dev = t->device;
    free(t);
    d3d_release(dev);
}

static uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) m >>= 1, n++;
    return n;
}

/* Create a texture of kind; *out the guest object. */
static uint32_t create(uint32_t dev, enum d3d_kind kind, uint32_t w, uint32_t h, uint32_t dpt, uint32_t levels,
                       uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t *out) {
    struct d3d_device *d = d3d_dev(dev);
    uint32_t rtype = kind == D3DK_TEXTURE ? D3DRTYPE_TEXTURE_ : kind == D3DK_CUBETEXTURE ? D3DRTYPE_CUBETEXTURE_
                   : D3DRTYPE_VOLUMETEXTURE_;
    if (!out || !w || !h || !dpt || w > 4096 || h > 4096 || pool > D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if (kind == D3DK_VOLUMETEXTURE && (w > 512 || h > 512 || dpt > 512)) return D3DERR_INVALIDCALL;
    if (usage & D3DUSAGE_DEPTHSTENCIL) return D3DERR_INVALIDCALL;
    if ((usage & D3DUSAGE_RENDERTARGET) && pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    if ((usage & D3DUSAGE_DYNAMIC) && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    uint32_t chk = d3d_check_format(D3DFMT_X8R8G8B8, usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP), rtype, fmt);
    if (chk != D3D_OK && chk != D3DOK_NOAUTOGEN) return D3DERR_INVALIDCALL;
    uint32_t chain = full_chain(w, h, kind == D3DK_VOLUMETEXTURE ? dpt : 1);
    int autogen = (usage & D3DUSAGE_AUTOGENMIPMAP) && chk == D3D_OK && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH;
    if (usage & D3DUSAGE_AUTOGENMIPMAP) {
        if (levels > 1) return D3DERR_INVALIDCALL;
        levels = 1;
    } else if (!levels) levels = chain;
    else if (levels > chain) return D3DERR_INVALIDCALL;
    struct d3d_texture *t = calloc(1, sizeof *t);
    t->base.destroy = destroy_texture;
    t->device = dev, t->fmt = fmt, t->usage = usage, t->pool = pool;
    t->w = w, t->h = kind == D3DK_CUBETEXTURE ? w : h, t->d = kind == D3DK_VOLUMETEXTURE ? dpt : 1;
    t->levels = levels, t->faces = kind == D3DK_CUBETEXTURE ? 6 : 1, t->f = d3d_format(fmt);
    t->autogen = autogen, t->mtl_levels = autogen ? chain : levels;
    t->autogen_filter = 2;                               /* D3DTEXF_LINEAR */
    uint32_t n = t->faces * levels;
    t->mem = calloc(n, 4), t->child = calloc(n, 4), t->locked = calloc(n, sizeof(int));
    t->lock_ro = calloc(n, sizeof(int)), t->lockbox = calloc(n, sizeof *t->lockbox);
    if (pool == D3DPOOL_DEFAULT || pool == D3DPOOL_MANAGED) {
        enum mtl_textype mt = kind == D3DK_CUBETEXTURE ? MT_CUBE : kind == D3DK_VOLUMETEXTURE ? MT_3D : MT_2D;
        uint32_t flags = (usage & D3DUSAGE_RENDERTARGET ? MT_RENDER : 0) | (autogen ? MT_MIPGEN : 0);
        t->tex = mtl_tex_new(d->mtl, t->f->store, mt, t->w, t->h, t->d, t->mtl_levels, flags);
        t->view = mtl_tex_view(t->tex, t->f->swizzle, 0);
    }
    t->counts_for_reset = pool == D3DPOOL_DEFAULT;
    if (t->counts_for_reset) d->default_pool_live++;
    d3d_addref(dev);
    const char *iface = kind == D3DK_TEXTURE ? "IDirect3DTexture9" : kind == D3DK_CUBETEXTURE
                      ? "IDirect3DCubeTexture9" : "IDirect3DVolumeTexture9";
    *out = d3d_new(kind, iface, &t->base);
    return D3D_OK;
}

static void dv_CreateTexture(void) {       /* (this, w, h, levels, usage, fmt, pool, IDirect3DTexture9**, shared) */
    uint32_t o = 0, hr = A32(8) ? D3DERR_INVALIDCALL
                       : create(A32(0), D3DK_TEXTURE, A32(1), A32(2), 1, A32(3), A32(4), A32(5), A32(6), &o);
    if (A32(7)) MEM32(A32(7)) = o;
    RET(hr, 9);
}
static void dv_CreateVolumeTexture(void) { /* (this, w, h, d, levels, usage, fmt, pool, pp, shared) */
    uint32_t o = 0, hr = A32(9) ? D3DERR_INVALIDCALL
                       : create(A32(0), D3DK_VOLUMETEXTURE, A32(1), A32(2), A32(3), A32(4), A32(5), A32(6), A32(7), &o);
    if (A32(8)) MEM32(A32(8)) = o;
    RET(hr, 10);
}
static void dv_CreateCubeTexture(void) {   /* (this, edge, levels, usage, fmt, pool, pp, shared) */
    uint32_t o = 0, hr = A32(7) ? D3DERR_INVALIDCALL
                       : create(A32(0), D3DK_CUBETEXTURE, A32(1), A32(1), 1, A32(2), A32(3), A32(4), A32(5), &o);
    if (A32(6)) MEM32(A32(6)) = o;
    RET(hr, 8);
}

mtl_obj d3d_tex_srgb_view(struct d3d_texture *t) {
    if (!t->srgb_view && t->tex) t->srgb_view = mtl_tex_view(t->tex, t->f->swizzle, (t->f->use & FU_SRGB) != 0);
    return t->srgb_view ? t->srgb_view : t->view;
}

/* ---- shared methods ---- */
static void rs_GetDevice(void) {           /* (this, IDirect3DDevice9**) */
    struct d3d_obj *o = d3d_host_any(A32(0));
    if (!o) d3d_fail("GetDevice on dead object 0x%08X", A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    uint32_t dev = is_tex_kind(o->kind) ? ((struct d3d_texture *)o)->device : d3d_buf(A32(0))->device;
    d3d_addref(dev);
    MEM32(A32(1)) = dev;
    RET(D3D_OK, 2);
}
static void tx_SetPriority(void) {
    struct d3d_texture *t = d3d_tex(A32(0));
    uint32_t old = t->priority;
    if (t->pool == D3DPOOL_MANAGED) t->priority = A32(1);
    RET(old, 2);
}
static void tx_GetPriority(void) { RET(d3d_tex(A32(0))->priority, 1); }
static void tx_PreLoad(void) { (void)d3d_tex(A32(0)); RET(0, 1); }
static void tx_GetType(void) {
    struct d3d_texture *t = d3d_tex(A32(0));
    RET(t->base.kind == D3DK_TEXTURE ? D3DRTYPE_TEXTURE_ : t->base.kind == D3DK_CUBETEXTURE ? D3DRTYPE_CUBETEXTURE_
        : D3DRTYPE_VOLUMETEXTURE_, 1);
}
static void tx_SetLOD(void) {
    struct d3d_texture *t = d3d_tex(A32(0));
    uint32_t old = t->lod;
    if (t->pool == D3DPOOL_MANAGED) t->lod = A32(1) < t->levels ? A32(1) : t->levels - 1;
    RET(old, 2);
}
static void tx_GetLOD(void) { RET(d3d_tex(A32(0))->lod, 1); }
static void tx_GetLevelCount(void) { RET(d3d_tex(A32(0))->levels, 1); }
static void tx_SetAutoGenFilterType(void) {
    struct d3d_texture *t = d3d_tex(A32(0));
    if (A32(1) == 0) RET(D3DERR_INVALIDCALL, 2);
    t->autogen_filter = A32(1);
    RET(D3D_OK, 2);
}
static void tx_GetAutoGenFilterType(void) { RET(d3d_tex(A32(0))->autogen_filter, 1); }
static void tx_GenerateMipSubLevels(void) {
    struct d3d_texture *t = d3d_tex(A32(0));
    if (t->autogen) mtl_tex_mipgen(((struct d3d_device *)d3d_host(t->device, D3DK_DEVICE))->mtl, t->tex);
    RET(0, 1);
}

static void put_level_desc(struct d3d_texture *t, uint32_t level, uint32_t p) {
    MEM32(p) = t->fmt, MEM32(p + 4) = 1, MEM32(p + 8) = t->usage, MEM32(p + 12) = t->pool;
    MEM32(p + 16) = 0, MEM32(p + 20) = 0, MEM32(p + 24) = dim(t->w, level), MEM32(p + 28) = dim(t->h, level);
}

/* The surface of one 2D level (face 0 for a texture). */
static uint32_t level_surface(struct d3d_texture *t, uint32_t face, uint32_t level) {
    uint32_t sub = face * t->levels + level;
    if (t->child[sub]) { d3d_addref(t->child[sub]); return t->child[sub]; }
    uint32_t s = d3d_surface_new(t->device, t->base.guest, t->fmt, t->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC),
                                 t->pool, dim(t->w, level), dim(t->h, level), t->tex, face, level, 0);
    struct d3d_surface *ss = d3d_surf(s);
    ss->parent = t, ss->sub = sub;
    t->child[sub] = s;
    return s;
}

static uint32_t lock2d(struct d3d_texture *t, uint32_t face, uint32_t level, uint32_t lr, uint32_t rect, uint32_t flags) {
    if (level >= t->levels || !lr) return D3DERR_INVALIDCALL;
    if (!(t->pool == D3DPOOL_MANAGED || t->pool == D3DPOOL_SYSTEMMEM || t->pool == D3DPOOL_SCRATCH ||
          (t->usage & D3DUSAGE_DYNAMIC)))
        return D3DERR_INVALIDCALL;
    uint32_t sub = face * t->levels + level;
    if (t->locked[sub]) return D3DERR_INVALIDCALL;
    uint32_t lw = dim(t->w, level), lh = dim(t->h, level);
    struct d3d_rect r = { 0, 0, (int32_t)lw, (int32_t)lh };
    if (rect) memcpy(&r, hle_ptr(rect), sizeof r);
    if (r.l < 0 || r.t < 0 || r.r > (int32_t)lw || r.b > (int32_t)lh || r.l >= r.r || r.t >= r.b)
        return D3DERR_INVALIDCALL;
    uint32_t bs = t->f->block, pitch = level_pitch(t, level);
    MEM32(lr) = pitch;
    MEM32(lr + 4) = d3d_tex_mem(t, sub) + ((uint32_t)r.t / bs) * pitch + ((uint32_t)r.l / bs) * t->f->bytes;
    t->locked[sub] = 1, t->lock_ro[sub] = (flags & D3DLOCK_READONLY) != 0;
    t->lockbox[sub] = (struct d3d_box){ (uint32_t)r.l, (uint32_t)r.t, (uint32_t)r.r, (uint32_t)r.b, 0, 1 };
    return D3D_OK;
}
static uint32_t unlock(struct d3d_texture *t, uint32_t face, uint32_t level) {
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    uint32_t sub = face * t->levels + level;
    if (!t->locked[sub]) return D3DERR_INVALIDCALL;
    t->locked[sub] = 0;
    if (!t->lock_ro[sub]) d3d_tex_upload(t, sub, &t->lockbox[sub]);
    return D3D_OK;
}

/* ---- IDirect3DTexture9 ---- */
static void t2_GetLevelDesc(void) {        /* (this, level, D3DSURFACE_DESC*) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_TEXTURE);
    if (A32(1) >= t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    put_level_desc(t, A32(1), A32(2));
    RET(D3D_OK, 3);
}
static void t2_GetSurfaceLevel(void) {     /* (this, level, IDirect3DSurface9**) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_TEXTURE);
    if (A32(1) >= t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = level_surface(t, 0, A32(1));
    RET(D3D_OK, 3);
}
static void t2_LockRect(void) {            /* (this, level, D3DLOCKED_RECT*, RECT*, flags) */
    RET(lock2d(tex_of_kind(A32(0), D3DK_TEXTURE), 0, A32(1), A32(2), A32(3), A32(4)), 5);
}
static void t2_UnlockRect(void) { RET(unlock(tex_of_kind(A32(0), D3DK_TEXTURE), 0, A32(1)), 2); }
static void t2_AddDirtyRect(void) { (void)tex_of_kind(A32(0), D3DK_TEXTURE); RET(D3D_OK, 2); }

/* ---- IDirect3DCubeTexture9 ---- */
static void tc_GetLevelDesc(void) {
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_CUBETEXTURE);
    if (A32(1) >= t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    put_level_desc(t, A32(1), A32(2));
    RET(D3D_OK, 3);
}
static void tc_GetCubeMapSurface(void) {   /* (this, face, level, IDirect3DSurface9**) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_CUBETEXTURE);
    if (A32(1) >= 6 || A32(2) >= t->levels || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    MEM32(A32(3)) = level_surface(t, A32(1), A32(2));
    RET(D3D_OK, 4);
}
static void tc_LockRect(void) {            /* (this, face, level, D3DLOCKED_RECT*, RECT*, flags) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_CUBETEXTURE);
    if (A32(1) >= 6) RET(D3DERR_INVALIDCALL, 6);
    RET(lock2d(t, A32(1), A32(2), A32(3), A32(4), A32(5)), 6);
}
static void tc_UnlockRect(void) {          /* (this, face, level) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_CUBETEXTURE);
    if (A32(1) >= 6) RET(D3DERR_INVALIDCALL, 3);
    RET(unlock(t, A32(1), A32(2)), 3);
}
static void tc_AddDirtyRect(void) { (void)tex_of_kind(A32(0), D3DK_CUBETEXTURE); RET(D3D_OK, 3); }

/* ---- IDirect3DVolumeTexture9 and IDirect3DVolume9 ---- */
static void put_volume_desc(struct d3d_texture *t, uint32_t level, uint32_t p) {
    MEM32(p) = t->fmt, MEM32(p + 4) = D3DRTYPE_VOLUME_, MEM32(p + 8) = t->usage, MEM32(p + 12) = t->pool;
    MEM32(p + 16) = dim(t->w, level), MEM32(p + 20) = dim(t->h, level), MEM32(p + 24) = dim(t->d, level);
}
static uint32_t lock_box(struct d3d_texture *t, uint32_t level, uint32_t lb, uint32_t boxva, uint32_t flags) {
    if (level >= t->levels || !lb || t->locked[level]) return D3DERR_INVALIDCALL;
    if (!(t->pool == D3DPOOL_MANAGED || t->pool == D3DPOOL_SYSTEMMEM || t->pool == D3DPOOL_SCRATCH ||
          (t->usage & D3DUSAGE_DYNAMIC)))
        return D3DERR_INVALIDCALL;
    struct d3d_box b = { 0, 0, dim(t->w, level), dim(t->h, level), 0, dim(t->d, level) };
    if (boxva) memcpy(&b, hle_ptr(boxva), sizeof b);
    if (b.r > dim(t->w, level) || b.b > dim(t->h, level) || b.back > dim(t->d, level) || b.l >= b.r || b.t >= b.b ||
        b.front >= b.back)
        return D3DERR_INVALIDCALL;
    uint32_t bs = t->f->block, pitch = level_pitch(t, level), slice = pitch * level_rows(t, level);
    MEM32(lb) = pitch, MEM32(lb + 4) = slice;
    MEM32(lb + 8) = d3d_tex_mem(t, level) + b.front * slice + (b.t / bs) * pitch + (b.l / bs) * t->f->bytes;
    t->locked[level] = 1, t->lock_ro[level] = (flags & D3DLOCK_READONLY) != 0, t->lockbox[level] = b;
    return D3D_OK;
}
static void tv_GetLevelDesc(void) {        /* (this, level, D3DVOLUME_DESC*) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_VOLUMETEXTURE);
    if (A32(1) >= t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    put_volume_desc(t, A32(1), A32(2));
    RET(D3D_OK, 3);
}
struct d3d_volume { struct d3d_obj base; struct d3d_texture *t; uint32_t level; };
static void destroy_volume(struct d3d_obj *o) {
    struct d3d_volume *v = (struct d3d_volume *)o;
    v->t->child[v->level] = 0;
    uint32_t tex = v->t->base.guest;
    free(v);
    d3d_release(tex);
}
static void tv_GetVolumeLevel(void) {      /* (this, level, IDirect3DVolume9**) */
    struct d3d_texture *t = tex_of_kind(A32(0), D3DK_VOLUMETEXTURE);
    uint32_t l = A32(1);
    if (l >= t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    if (t->child[l]) d3d_addref(t->child[l]);
    else {
        struct d3d_volume *v = calloc(1, sizeof *v);
        v->base.destroy = destroy_volume, v->t = t, v->level = l;
        d3d_addref(t->base.guest);
        t->child[l] = d3d_new(D3DK_VOLUME, "IDirect3DVolume9", &v->base);
    }
    MEM32(A32(2)) = t->child[l];
    RET(D3D_OK, 3);
}
static void tv_LockBox(void) {             /* (this, level, D3DLOCKED_BOX*, D3DBOX*, flags) */
    RET(lock_box(tex_of_kind(A32(0), D3DK_VOLUMETEXTURE), A32(1), A32(2), A32(3), A32(4)), 5);
}
static void tv_UnlockBox(void) { RET(unlock(tex_of_kind(A32(0), D3DK_VOLUMETEXTURE), 0, A32(1)), 2); }
static void tv_AddDirtyBox(void) { (void)tex_of_kind(A32(0), D3DK_VOLUMETEXTURE); RET(D3D_OK, 2); }

static struct d3d_volume *vol(uint32_t o) { return d3d_host(o, D3DK_VOLUME); }
static void vl_GetDevice(void) {
    struct d3d_volume *v = vol(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(v->t->device);
    MEM32(A32(1)) = v->t->device;
    RET(D3D_OK, 2);
}
static void vl_GetContainer(void) { struct d3d_volume *v = vol(A32(0)); RET(d3d_qi(v->t->base.guest, APTR(1), A32(2)), 3); }
static void vl_GetDesc(void) {
    struct d3d_volume *v = vol(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    put_volume_desc(v->t, v->level, A32(1));
    RET(D3D_OK, 2);
}
static void vl_LockBox(void) {             /* (this, D3DLOCKED_BOX*, D3DBOX*, flags) */
    struct d3d_volume *v = vol(A32(0));
    RET(lock_box(v->t, v->level, A32(1), A32(2), A32(3)), 4);
}
static void vl_UnlockBox(void) { struct d3d_volume *v = vol(A32(0)); RET(unlock(v->t, 0, v->level), 1); }

/* ---- the device's bindings ---- */
static int smp_index(uint32_t s) {
    if (s < 16) return (int)s;
    if (s >= 257 && s <= 260) return 16 + (int)(s - 257);
    return -1;
}
static void dv_SetTexture(void) {          /* (this, stage, IDirect3DBaseTexture9*) */
    struct d3d_device *d = d3d_dev(A32(0));
    int i = smp_index(A32(1));
    if (i < 0) RET(D3DERR_INVALIDCALL, 3);
    if (A32(2)) {
        struct d3d_texture *t = d3d_tex(A32(2));
        if (t->pool == D3DPOOL_SYSTEMMEM || t->pool == D3DPOOL_SCRATCH) RET(D3DERR_INVALIDCALL, 3);
    }
    if (d->st.tex[i] != A32(2)) {
        d3d_bind(&d->st.tex[i], A32(2));
        d->st.dirty |= DIRTY_SAMPLER | DIRTY_STAGE;
    }
    RET(D3D_OK, 3);
}
static void dv_GetTexture(void) {          /* (this, stage, IDirect3DBaseTexture9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    int i = smp_index(A32(1));
    if (i < 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d->st.tex[i];
    if (d->st.tex[i]) d3d_addref(d->st.tex[i]);
    RET(D3D_OK, 3);
}

#define TEXCOMMON(I) \
    { I "::GetDevice", rs_GetDevice }, { I "::SetPriority", tx_SetPriority }, { I "::GetPriority", tx_GetPriority }, \
    { I "::PreLoad", tx_PreLoad }, { I "::GetType", tx_GetType }, { I "::SetLOD", tx_SetLOD }, \
    { I "::GetLOD", tx_GetLOD }, { I "::GetLevelCount", tx_GetLevelCount }, \
    { I "::SetAutoGenFilterType", tx_SetAutoGenFilterType }, { I "::GetAutoGenFilterType", tx_GetAutoGenFilterType }, \
    { I "::GenerateMipSubLevels", tx_GenerateMipSubLevels }
static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::CreateTexture", dv_CreateTexture },
    { "IDirect3DDevice9::CreateVolumeTexture", dv_CreateVolumeTexture },
    { "IDirect3DDevice9::CreateCubeTexture", dv_CreateCubeTexture },
    { "IDirect3DDevice9::SetTexture", dv_SetTexture },
    { "IDirect3DDevice9::GetTexture", dv_GetTexture },
    TEXCOMMON("IDirect3DTexture9"), TEXCOMMON("IDirect3DCubeTexture9"), TEXCOMMON("IDirect3DVolumeTexture9"),
    { "IDirect3DTexture9::GetLevelDesc", t2_GetLevelDesc },
    { "IDirect3DTexture9::GetSurfaceLevel", t2_GetSurfaceLevel },
    { "IDirect3DTexture9::LockRect", t2_LockRect },
    { "IDirect3DTexture9::UnlockRect", t2_UnlockRect },
    { "IDirect3DTexture9::AddDirtyRect", t2_AddDirtyRect },
    { "IDirect3DCubeTexture9::GetLevelDesc", tc_GetLevelDesc },
    { "IDirect3DCubeTexture9::GetCubeMapSurface", tc_GetCubeMapSurface },
    { "IDirect3DCubeTexture9::LockRect", tc_LockRect },
    { "IDirect3DCubeTexture9::UnlockRect", tc_UnlockRect },
    { "IDirect3DCubeTexture9::AddDirtyRect", tc_AddDirtyRect },
    { "IDirect3DVolumeTexture9::GetLevelDesc", tv_GetLevelDesc },
    { "IDirect3DVolumeTexture9::GetVolumeLevel", tv_GetVolumeLevel },
    { "IDirect3DVolumeTexture9::LockBox", tv_LockBox },
    { "IDirect3DVolumeTexture9::UnlockBox", tv_UnlockBox },
    { "IDirect3DVolumeTexture9::AddDirtyBox", tv_AddDirtyBox },
    { "IDirect3DVolume9::GetDevice", vl_GetDevice },
    { "IDirect3DVolume9::GetContainer", vl_GetContainer },
    { "IDirect3DVolume9::GetDesc", vl_GetDesc },
    { "IDirect3DVolume9::LockBox", vl_LockBox },
    { "IDirect3DVolume9::UnlockBox", vl_UnlockBox },
    { 0, 0 } };
void d3d9_texture_register(void) { d3d_impls(g_impls); }
