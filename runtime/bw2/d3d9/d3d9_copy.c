/*
 * d3d9_copy.c - UpdateSurface, UpdateTexture, StretchRect and ColorFill
 * (plan 4 Task 5). All of them are recorded in command order.
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

static struct d3d_device *dev_of(struct d3d_surface *s) { return d3d_host(s->device, D3DK_DEVICE); }

static void dv_StretchRect(void) {         /* (this, src, RECT* src, dst, RECT* dst, filter) */
    struct d3d_device *d = d3d_dev(A32(0));
    struct d3d_surface *s = d3d_surf(A32(1)), *t = d3d_surf(A32(3));
    uint32_t filter = A32(5);
    if (!s->tex || !t->tex || s->pool != D3DPOOL_DEFAULT && !s->parent) RET(D3DERR_INVALIDCALL, 6);
    if (d3d_format_is_depth(s->f) || d3d_format_is_depth(t->f))
        d3d_fail("StretchRect between depth surfaces is not supported");
    if (s->f->block != 1 || t->f->block != 1) RET(D3DERR_INVALIDCALL, 6);
    struct d3d_rect sr = { 0, 0, (int32_t)s->w, (int32_t)s->h }, dr = { 0, 0, (int32_t)t->w, (int32_t)t->h };
    if (A32(2)) memcpy(&sr, APTR(2), sizeof sr);
    if (A32(4)) memcpy(&dr, APTR(4), sizeof dr);
    if (sr.l < 0 || sr.t < 0 || sr.r > (int32_t)s->w || sr.b > (int32_t)s->h || sr.l >= sr.r || sr.t >= sr.b ||
        dr.l < 0 || dr.t < 0 || dr.r > (int32_t)t->w || dr.b > (int32_t)t->h || dr.l >= dr.r || dr.t >= dr.b)
        RET(D3DERR_INVALIDCALL, 6);
    uint32_t w = (uint32_t)(sr.r - sr.l), h = (uint32_t)(sr.b - sr.t);
    if (s->fmt == t->fmt && w == (uint32_t)(dr.r - dr.l) && h == (uint32_t)(dr.b - dr.t)) {
        mtl_tex_copy(d->mtl, s->tex, s->slice, s->level, (uint32_t)sr.l, (uint32_t)sr.t, t->tex, t->slice, t->level,
                     (uint32_t)dr.l, (uint32_t)dr.t, w, h);
    } else {
        struct mtl_rect a = { (uint32_t)sr.l, (uint32_t)sr.t, w, h };
        struct mtl_rect b = { (uint32_t)dr.l, (uint32_t)dr.t, (uint32_t)(dr.r - dr.l), (uint32_t)(dr.b - dr.t) };
        struct mtl_target dst = { t->tex, t->slice, t->level };
        mtl_stretch(d->mtl, s->tex, s->f->swizzle, s->slice, s->level, &a, dst, &b, filter == 2);
    }
    if (t->parent && t->parent->autogen && t->level == 0) mtl_tex_mipgen(d->mtl, t->tex);
    RET(D3D_OK, 6);
}

static void dv_ColorFill(void) {           /* (this, surface, RECT*, color) */
    struct d3d_device *d = d3d_dev(A32(0));
    struct d3d_surface *s = d3d_surf(A32(1));
    if (!s->tex || s->pool != D3DPOOL_DEFAULT || d3d_format_is_depth(s->f) || s->f->block != 1)
        RET(D3DERR_INVALIDCALL, 4);
    struct d3d_rect r = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    if (A32(2)) memcpy(&r, APTR(2), sizeof r);
    if (r.l < 0 || r.t < 0 || r.r > (int32_t)s->w || r.b > (int32_t)s->h || r.l >= r.r || r.t >= r.b)
        RET(D3DERR_INVALIDCALL, 4);
    struct mtl_target rt[4] = { { s->tex, s->slice, s->level } }, none = { 0 };
    mtl_set_targets(d->mtl, rt, none);
    struct mtl_rect mr = { (uint32_t)r.l, (uint32_t)r.t, (uint32_t)(r.r - r.l), (uint32_t)(r.b - r.t) };
    mtl_clear(d->mtl, 1, A32(3), 0, 0, &mr, 1);
    d->st.dirty |= DIRTY_TARGET;                         /* the next draw puts the device's targets back */
    RET(D3D_OK, 4);
}

/* Copy rows of one subresource's D3D layout into another's. */
static void copy_rows(const struct d3d_format *f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t dpitch,
                      uint32_t w, uint32_t rows) {
    uint32_t n = d3d_format_pitch(f, w);
    for (uint32_t y = 0; y < rows; y++) memcpy(dst + y * dpitch, src + y * spitch, n);
}

static void dv_UpdateSurface(void) {       /* (this, src (system memory), RECT*, dst (default), POINT*) */
    (void)d3d_dev(A32(0));
    struct d3d_surface *s = d3d_surf(A32(1)), *t = d3d_surf(A32(3));
    if (s->pool != D3DPOOL_SYSTEMMEM || t->pool != D3DPOOL_DEFAULT || s->fmt != t->fmt) RET(D3DERR_INVALIDCALL, 5);
    struct d3d_rect sr = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    if (A32(2)) memcpy(&sr, APTR(2), sizeof sr);
    int32_t dx = A32(4) ? (int32_t)MEM32(A32(4)) : 0, dy = A32(4) ? (int32_t)MEM32(A32(4) + 4) : 0;
    uint32_t w = (uint32_t)(sr.r - sr.l), h = (uint32_t)(sr.b - sr.t), bs = s->f->block;
    if (sr.l < 0 || sr.t < 0 || sr.r > (int32_t)s->w || sr.b > (int32_t)s->h || !w || !h || dx < 0 || dy < 0 ||
        dx + w > t->w || dy + h > t->h || ((sr.l | sr.t | dx | dy) % (int32_t)bs))
        RET(D3DERR_INVALIDCALL, 5);
    const uint8_t *src = (const uint8_t *)hle_ptr(d3d_surface_mem(s)) + ((uint32_t)sr.t / bs) * s->pitch +
                         ((uint32_t)sr.l / bs) * s->f->bytes;
    uint8_t *dst = (uint8_t *)hle_ptr(d3d_surface_mem(t)) + ((uint32_t)dy / bs) * t->pitch + ((uint32_t)dx / bs) * t->f->bytes;
    copy_rows(s->f, src, s->pitch, dst, t->pitch, w, d3d_format_rows(s->f, h));
    struct d3d_rect r = { dx, dy, dx + (int32_t)w, dy + (int32_t)h };
    d3d_surface_upload(t, &r);
    RET(D3D_OK, 5);
}

static void dv_UpdateTexture(void) {       /* (this, src (system memory), dst (default)) */
    (void)d3d_dev(A32(0));
    struct d3d_texture *s = d3d_tex(A32(1)), *t = d3d_tex(A32(2));
    if (s->pool != D3DPOOL_SYSTEMMEM || t->pool != D3DPOOL_DEFAULT || s->base.kind != t->base.kind ||
        s->fmt != t->fmt || s->levels < (t->autogen ? 1 : t->levels))
        RET(D3DERR_INVALIDCALL, 3);
    /* the source level matching the destination's top level */
    uint32_t skip = 0;
    while (skip < s->levels && d3d_tex_level_w(s, skip) > t->w) skip++;
    if (skip >= s->levels || d3d_tex_level_w(s, skip) != t->w || d3d_tex_level_h(s, skip) != t->h)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t nl = t->autogen ? 1 : t->levels;
    for (uint32_t face = 0; face < t->faces; face++)
        for (uint32_t l = 0; l < nl; l++) {
            uint32_t ss = face * s->levels + l + skip, ts = face * t->levels + l;
            uint32_t pitch = d3d_format_pitch(t->f, d3d_tex_level_w(t, l));
            uint32_t rows = d3d_format_rows(t->f, d3d_tex_level_h(t, l)) * d3d_tex_level_d(t, l);
            memcpy(hle_ptr(d3d_tex_mem(t, ts)), hle_ptr(d3d_tex_mem(s, ss)), (size_t)pitch * rows);
            struct d3d_box b = { 0, 0, d3d_tex_level_w(t, l), d3d_tex_level_h(t, l), 0, d3d_tex_level_d(t, l) };
            d3d_tex_upload(t, ts, &b);
        }
    RET(D3D_OK, 3);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::StretchRect", dv_StretchRect },
    { "IDirect3DDevice9::ColorFill", dv_ColorFill },
    { "IDirect3DDevice9::UpdateSurface", dv_UpdateSurface },
    { "IDirect3DDevice9::UpdateTexture", dv_UpdateTexture },
    { 0, 0 } };
void d3d9_copy_register(void) { d3d_impls(g_impls); }
