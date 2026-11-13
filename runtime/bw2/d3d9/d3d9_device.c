/*
 * d3d9_device.c - IDirect3D9::CreateDevice, IDirect3DDevice9's device-level
 * methods and the implicit swap chain (plan 4 Task 4).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bw2.h"
#include "d3d9_adapter.h"
#include "d3d9_device.h"
#include "ff.h"

#define D3DCREATE_FPU_PRESERVE 0x2u
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000u
#define D3DBACKBUFFER_TYPE_MONO 0u

struct d3d_device *g_d3d_cur;

struct d3d_device *d3d_dev(uint32_t obj) {
    struct d3d_device *d = d3d_host(obj, D3DK_DEVICE);
    g_d3d_cur = d;
    return d;
}
struct d3d_state *d3d_cur(void) { return g_d3d_cur ? &g_d3d_cur->st : NULL; }

void d3d_bind(uint32_t *slot, uint32_t obj) {
    if (*slot == obj) return;
    if (obj) d3d_addref(obj);
    uint32_t old = *slot;
    *slot = obj;
    if (old) d3d_release(old);
}

/* ---- swap chains: the implicit one (backbuffer 0: the device's) and additional ones ---- */
struct d3d_swapchain { struct d3d_obj base; uint32_t device, backbuffer, pp[14]; };
static void destroy_swapchain(struct d3d_obj *o) {
    struct d3d_swapchain *sc = (struct d3d_swapchain *)o;
    uint32_t dev = sc->backbuffer ? sc->device : 0;
    if (sc->backbuffer) d3d_release(sc->backbuffer);
    if (dev && d3d_alive(dev)) {
        ((struct d3d_device *)d3d_host(dev, D3DK_DEVICE))->default_pool_live--;
        free(sc);
        d3d_release(dev);
        return;
    }
    free(sc);
}

/* ---- creation and Reset ---- */
static void window_client(uint32_t hwnd, uint32_t *w, uint32_t *h) {
    *w = 640, *h = 480;
    uint32_t fn = hle_resolve("GetClientRect");
    if (!hwnd || !fn) return;
    uint32_t mark = hle_scratch_mark(), r = hle_scratch(16);
    uint32_t a[2] = { hwnd, r };
    if (hle_call_guest(fn, 2, a) && MEM32(r + 8) > MEM32(r) && MEM32(r + 12) > MEM32(r + 4))
        *w = MEM32(r + 8) - MEM32(r), *h = MEM32(r + 12) - MEM32(r + 4);
    hle_scratch_release(mark);
}

/* Fill in and check the guest's D3DPRESENT_PARAMETERS (written back, as D3D does). */
static uint32_t check_pp(uint32_t focus, uint32_t ppva, uint32_t *pp) {
    memcpy(pp, hle_ptr(ppva), 56);
    uint32_t windowed = pp[8], hwnd = pp[7] ? pp[7] : focus;
    if (pp[4] != 0) d3d_fail("multisampling %u asked for (not supported)", pp[4]);
    if (pp[3] > 3) return D3DERR_INVALIDCALL;
    if (!pp[3]) pp[3] = 1;
    if (windowed) {
        if (!pp[0] || !pp[1]) {
            uint32_t w, h;
            window_client(hwnd, &w, &h);
            if (!pp[0]) pp[0] = w;
            if (!pp[1]) pp[1] = h;
        }
        if (pp[2] == D3DFMT_UNKNOWN) pp[2] = D3DFMT_X8R8G8B8;
    } else if (!pp[0] || !pp[1]) return D3DERR_INVALIDCALL;
    const struct d3d_format *bb = d3d_format(pp[2]);
    if (!bb || !(bb->use & FU_RT)) return D3DERR_INVALIDCALL;
    if (pp[9]) {
        const struct d3d_format *ds = d3d_format(pp[10]);
        if (!ds || !(ds->use & FU_DS)) return D3DERR_INVALIDCALL;
    }
    memcpy(hle_ptr(ppva), pp, 56);
    return D3D_OK;
}

static void make_targets(struct d3d_device *d) {
    d->backbuffer = d3d_surface_new(d->base.guest, 0, d->pp[2], D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, d->pp[0],
                                    d->pp[1], NULL, 0, 0, 0);
    d->auto_ds = d->pp[9] ? d3d_surface_new(d->base.guest, 0, d->pp[10], D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT,
                                            d->pp[0], d->pp[1], NULL, 0, 0, 0) : 0;
    d3d_bind(&d->rt[0], d->backbuffer);
    d3d_bind(&d->ds, d->auto_ds);
}
static void drop_targets(struct d3d_device *d) {
    for (int i = 0; i < 4; i++) d3d_bind(&d->rt[i], 0);
    d3d_bind(&d->ds, 0);
    if (d->backbuffer) d3d_release(d->backbuffer);
    if (d->auto_ds) d3d_release(d->auto_ds);
    d->backbuffer = d->auto_ds = 0;
}
static void unbind_all(struct d3d_device *d) {
    for (int i = 0; i < NSMP; i++) d3d_bind(&d->st.tex[i], 0);
    for (int i = 0; i < 16; i++) d3d_bind(&d->st.stream[i], 0);
    d3d_bind(&d->st.indices, 0);
    d3d_bind(&d->st.vdecl, 0);
    d3d_bind(&d->st.vs, 0);
    d3d_bind(&d->st.ps, 0);
}

static void destroy_device(struct d3d_obj *o) {
    struct d3d_device *d = (struct d3d_device *)o;
    unbind_all(d);
    d3d_bind(&d->last_presented, 0);
    free(d->palettes);
    drop_targets(d);
    if (d->swapchain) d3d_release(d->swapchain);
    mtl_flush(d->mtl);
    mtl_heap_forget_ctx(d->mtl);
    mtl_archive_save();
    mtl_ctx_free(d->mtl);
    d3d_mode_device(d, 0, 0, 0, 1);              /* a full-screen device's mode goes with it */
    d3d_state_free(&d->st);
    if (g_d3d_cur == d) g_d3d_cur = NULL;
    uint32_t d3d9 = d->d3d9;
    free(d);
    d3d_release(d3d9);
}

uint32_t d3d9_create_device(uint32_t d3d9, uint32_t adapter, uint32_t type, uint32_t focus, uint32_t flags,
                            uint32_t ppva, uint32_t *out) {
    if (adapter != 0 || !ppva || !out) return D3DERR_INVALIDCALL;
    if (type != 1) return D3DERR_NOTAVAILABLE;
    uint32_t pp[14];
    uint32_t hr = check_pp(focus, ppva, pp);
    if (hr) return hr;
    if (!(flags & D3DCREATE_FPU_PRESERVE)) g_fpu_cw &= (uint16_t)~0x0F00u;   /* 24-bit, round to nearest */
    static int archive;
    if (!archive) {                                  /* <support>/ShaderCache/ (my call) */
        const char *sup = getenv("BW2_SUPPORT"), *home = getenv("HOME");
        char path[1024];
        if (sup && *sup) snprintf(path, sizeof path, "%s/ShaderCache", sup);
        else snprintf(path, sizeof path, "%s/Library/Application Support/Black & White 2/ShaderCache",
                      home ? home : ".");
        mtl_archive_open(path);
        archive = 1;
    }
    struct d3d_device *d = calloc(1, sizeof *d);
    d->base.destroy = destroy_device;
    d->d3d9 = d3d9, d->adapter = adapter, d->devtype = type, d->focus = focus, d->flags = flags;
    memcpy(d->pp, pp, sizeof pp);
    /* the game window: hDeviceWindow, or the focus window when that is NULL (as D3D9 does) */
    d->mtl = mtl_ctx_new(pp[0], pp[1], pp[13] != D3DPRESENT_INTERVAL_IMMEDIATE, pp[7] ? pp[7] : focus, !pp[8]);
    d->cursor_shown = 0;
    for (int i = 0; i < 256; i++) d->gamma[i] = d->gamma[256 + i] = d->gamma[512 + i] = (uint16_t)(i * 257);
    uint32_t dev = d3d_new(D3DK_DEVICE, "IDirect3DDevice9", &d->base);
    d3d_addref(d3d9);
    struct d3d_swapchain *sc = calloc(1, sizeof *sc);
    sc->base.destroy = destroy_swapchain, sc->device = dev;
    d->swapchain = d3d_new(D3DK_SWAPCHAIN, "IDirect3DSwapChain9", &sc->base);
    make_targets(d);
    d3d_state_defaults(&d->st, pp[9] != 0, pp[0], pp[1]);
    g_d3d_cur = d;
    d3d_mode_device(d, !pp[8], pp[0], pp[1], 0);
    *out = dev;
    return D3D_OK;
}

static void a_CreateDevice(void) {        /* (this, adapter, type, focus, flags, pp, IDirect3DDevice9**) */
    (void)d3d_host(A32(0), D3DK_D3D9);
    uint32_t dev = 0, hr = d3d9_create_device(A32(0), A32(1), A32(2), A32(3), A32(4), A32(5), &dev);
    if (A32(6)) MEM32(A32(6)) = hr ? 0 : dev;
    RET(hr, 7);
}

/* ---- targets ---- */
static struct mtl_target target_of(uint32_t surf) {
    struct mtl_target t = { 0 };
    if (!surf) return t;
    struct d3d_surface *s = d3d_surf(surf);
    t.tex = s->tex, t.slice = s->slice, t.level = s->level;
    return t;
}
void d3d_apply_targets(struct d3d_device *d) {
    struct mtl_target rt[4];
    for (int i = 0; i < 4; i++) {
        rt[i] = target_of(d->rt[i]);
        if (d->rt[i]) {                              /* about to be drawn into: its mips go stale */
            struct d3d_surface *s = d3d_surf(d->rt[i]);
            if (s->parent && s->parent->autogen && s->level == 0) s->parent->mip_dirty = 1;
        }
    }
    mtl_set_targets(d->mtl, rt, target_of(d->ds));
}

static void dv_Clear(void) {               /* (this, count, D3DRECT*, flags, color, z, stencil) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t n = A32(1), rects = A32(2), flags = A32(3), color = A32(4), stencil = A32(6);
    float z;
    uint32_t zbits = A32(5);
    memcpy(&z, &zbits, 4);
    if ((flags & 6) && !d->ds) RET(D3DERR_INVALIDCALL, 7);
    if (n && !rects) RET(D3DERR_INVALIDCALL, 7);
    /* the area: the viewport, cut to the scissor rect when the test is on */
    int32_t l = (int32_t)d->st.vp.x, t = (int32_t)d->st.vp.y, r = l + (int32_t)d->st.vp.w, b = t + (int32_t)d->st.vp.h;
    if (d->st.rs[174]) {
        struct d3d_rect s = d->st.scissor;
        if (s.l > l) l = s.l;
        if (s.t > t) t = s.t;
        if (s.r < r) r = s.r;
        if (s.b < b) b = s.b;
    }
    struct mtl_rect *mr = malloc(sizeof *mr * (n ? n : 1));
    uint32_t k = 0;
    for (uint32_t i = 0; i < (n ? n : 1); i++) {
        int32_t rl = l, rt = t, rr = r, rb = b;
        if (n) {
            const int32_t *q = hle_ptr(rects + 16 * i);
            if (q[0] > rl) rl = q[0];
            if (q[1] > rt) rt = q[1];
            if (q[2] < rr) rr = q[2];
            if (q[3] < rb) rb = q[3];
        }
        if (rr > rl && rb > rt) mr[k++] = (struct mtl_rect){ (uint32_t)rl, (uint32_t)rt, (uint32_t)(rr - rl), (uint32_t)(rb - rt) };
    }
    d3d_apply_targets(d);
    if (d3d_surf(d->rt[0])->f->swizzle[3] == '1') color |= 0xFF000000u;   /* an X8 target's alpha stays one */
    mtl_clear(d->mtl, flags, color, z, stencil, mr, k);
    free(mr);
    RET(D3D_OK, 7);
}

/* The frame log (plan 7 Task 5): with BW2_FRAME_LOG=1 (read once) each Present reads the back buffer back and
 * prints its size, its mean RGB (0-255, truncated) and how many pixels are not black. No pixels are kept. */
static int frame_log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW2_FRAME_LOG");
        on = e && !strcmp(e, "1");
    }
    return on;
}
static void frame_log(struct d3d_device *d, struct d3d_surface *bb) {
    static uint32_t n;
    if (bb->f->store != ST_BGRA8)
        d3d_fail("frame log: back buffer format %s is not read back", bb->f->name);
    uint32_t w = bb->w, h = bb->h, pitch = w * 4;
    uint8_t *px = malloc((size_t)pitch * h);
    if (!px) d3d_fail("frame log: no memory for a %ux%u readback", w, h);
    mtl_tex_download(d->mtl, bb->tex, bb->slice, bb->level, 0, 0, w, h, px, pitch);
    uint64_t sum[3] = { 0, 0, 0 }, nonblack = 0;
    for (size_t i = 0, end = (size_t)w * h; i < end; i++) {
        const uint8_t *p = px + 4 * i;                       /* B, G, R, A */
        sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
        nonblack += (p[0] | p[1] | p[2]) != 0;
    }
    free(px);
    uint64_t total = (uint64_t)w * h;
    fprintf(stderr, "[bw2] frame %u: %ux%u, mean %llu,%llu,%llu, nonblack %llu of %llu\n", ++n, w, h,
            (unsigned long long)(sum[0] / total), (unsigned long long)(sum[1] / total),
            (unsigned long long)(sum[2] / total), (unsigned long long)nonblack, (unsigned long long)total);
}

void bw2_movie_present(void);           /* video/binkw32.c: BW2_QUIT_AFTER_MOVIE counts Presents */
void bw2_frame_dump(struct d3d_device *d, struct d3d_surface *bb);   /* frame_dump.m: BW2_FRAME_DUMP */
static void present_bb(struct d3d_device *d, uint32_t bbobj) {
    struct d3d_surface *bb = d3d_surf(bbobj);
    mtl_present(d->mtl, bb->tex);
    if (frame_log_on()) frame_log(d, bb);
    bw2_frame_dump(d, bb);
    bw2_movie_present();
    d3d_bind(&d->last_presented, bbobj);
}
static void present(struct d3d_device *d) { present_bb(d, d->backbuffer); }
static void dv_Present(void) {             /* (this, src, dst, hwnd, dirty) */
    present(d3d_dev(A32(0)));
    RET(D3D_OK, 5);
}

static void dv_GetBackBuffer(void) {       /* (this, swapchain, index, type, IDirect3DSurface9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) != 0 || A32(2) >= d->pp[3] || !A32(4)) RET(D3DERR_INVALIDCALL, 5);
    d3d_addref(d->backbuffer);
    MEM32(A32(4)) = d->backbuffer;
    RET(D3D_OK, 5);
}

static void dv_SetRenderTarget(void) {     /* (this, index, surface) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t i = A32(1), s = A32(2);
    if (i >= 4 || (i == 0 && !s)) RET(D3DERR_INVALIDCALL, 3);
    if (s && !(d3d_surf(s)->type_usage & D3DUSAGE_RENDERTARGET)) RET(D3DERR_INVALIDCALL, 3);
    d3d_bind(&d->rt[i], s);
    if (i == 0) {
        struct d3d_surface *rt = d3d_surf(s);
        d->st.vp = (struct d3d_viewport){ 0, 0, rt->w, rt->h, 0.0f, 1.0f };
        d->st.scissor = (struct d3d_rect){ 0, 0, (int32_t)rt->w, (int32_t)rt->h };
    }
    d->st.dirty |= DIRTY_TARGET | DIRTY_VIEW;
    RET(D3D_OK, 3);
}
static void dv_GetRenderTarget(void) {     /* (this, index, IDirect3DSurface9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t i = A32(1);
    if (i >= 4 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d->rt[i];
    if (!d->rt[i]) RET(D3DERR_NOTFOUND, 3);
    d3d_addref(d->rt[i]);
    RET(D3D_OK, 3);
}
static void dv_SetDepthStencilSurface(void) {   /* (this, surface) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t s = A32(1);
    if (s && !(d3d_surf(s)->type_usage & D3DUSAGE_DEPTHSTENCIL)) RET(D3DERR_INVALIDCALL, 2);
    d3d_bind(&d->ds, s);
    d->st.dirty |= DIRTY_TARGET | DIRTY_DS;
    RET(D3D_OK, 2);
}
static void dv_GetDepthStencilSurface(void) {   /* (this, IDirect3DSurface9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->ds;
    if (!d->ds) RET(D3DERR_NOTFOUND, 2);
    d3d_addref(d->ds);
    RET(D3D_OK, 2);
}

static void dv_CreateRenderTarget(void) {  /* (this, w, h, fmt, ms, msq, lockable, IDirect3DSurface9**, shared) */
    struct d3d_device *d = d3d_dev(A32(0));
    const struct d3d_format *f = d3d_format(A32(3));
    if (!A32(7) || !A32(1) || !A32(2) || !f || !(f->use & FU_RT) || A32(8)) RET(D3DERR_INVALIDCALL, 9);
    if (A32(4)) d3d_fail("multisampled render target (%u) asked for", A32(4));
    MEM32(A32(7)) = d3d_surface_new(d->base.guest, 0, A32(3), D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, A32(1), A32(2),
                                    NULL, 0, 0, 1);
    RET(D3D_OK, 9);
}
static void dv_CreateDepthStencilSurface(void) {   /* (this, w, h, fmt, ms, msq, discard, IDirect3DSurface9**, shared) */
    struct d3d_device *d = d3d_dev(A32(0));
    const struct d3d_format *f = d3d_format(A32(3));
    if (!A32(7) || !A32(1) || !A32(2) || !f || !(f->use & FU_DS) || A32(8)) RET(D3DERR_INVALIDCALL, 9);
    if (A32(4)) d3d_fail("multisampled depth surface (%u) asked for", A32(4));
    MEM32(A32(7)) = d3d_surface_new(d->base.guest, 0, A32(3), D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, A32(1), A32(2),
                                    NULL, 0, 0, 1);
    RET(D3D_OK, 9);
}
static void dv_CreateOffscreenPlainSurface(void) {   /* (this, w, h, fmt, pool, IDirect3DSurface9**, shared) */
    struct d3d_device *d = d3d_dev(A32(0));
    const struct d3d_format *f = d3d_format(A32(3));
    uint32_t pool = A32(4);
    if (!A32(5) || !A32(1) || !A32(2) || !f || (f->use & FU_DS) || pool == D3DPOOL_MANAGED || pool > 3 || A32(6))
        RET(D3DERR_INVALIDCALL, 7);
    MEM32(A32(5)) = d3d_surface_new(d->base.guest, 0, A32(3), 0, pool, A32(1), A32(2), NULL, 0, 0,
                                    pool == D3DPOOL_DEFAULT);
    RET(D3D_OK, 7);
}

static void dv_GetRenderTargetData(void) { /* (this, render target, system-memory surface) */
    (void)d3d_dev(A32(0));
    struct d3d_surface *rt = d3d_surf(A32(1)), *dst = d3d_surf(A32(2));
    if (!rt->tex || dst->pool != D3DPOOL_SYSTEMMEM || rt->w != dst->w || rt->h != dst->h || rt->fmt != dst->fmt)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t sp = d3d_storage_pitch(rt->f, rt->w), rows = d3d_format_rows(rt->f, rt->h);
    uint8_t *tmp = malloc((size_t)sp * rows);
    mtl_tex_download(((struct d3d_device *)d3d_host(rt->device, D3DK_DEVICE))->mtl,
                     rt->tex, rt->slice, rt->level, 0, 0, rt->w, rt->h, tmp, sp);
    d3d_from_storage(rt->f, tmp, sp, hle_ptr(d3d_surface_mem(dst)), dst->pitch, rt->w, rows);
    free(tmp);
    RET(D3D_OK, 3);
}

static void dv_Reset(void) {               /* (this, D3DPRESENT_PARAMETERS*) */
    struct d3d_device *d = d3d_dev(A32(0));
    /* The device's own bindings are D3D's internal references: they do not keep a released
     * resource alive through Reset. Drop them first, then count what the guest still holds. */
    unbind_all(d);
    for (int i = 1; i < 4; i++) d3d_bind(&d->rt[i], 0);
    d3d_bind(&d->rt[0], d->backbuffer);
    d3d_bind(&d->ds, d->auto_ds);
    d3d_bind(&d->last_presented, 0);
    if (d->default_pool_live) RET(D3DERR_INVALIDCALL, 2);
    uint32_t pp[14];
    uint32_t hr = check_pp(d->focus, A32(1), pp);
    if (hr) RET(hr, 2);
    mtl_flush(d->mtl);
    unbind_all(d);
    d3d_bind(&d->last_presented, 0);
    drop_targets(d);
    memcpy(d->pp, pp, sizeof pp);
    make_targets(d);
    d3d_state_defaults(&d->st, pp[9] != 0, pp[0], pp[1]);
    d->in_scene = 0;
    mtl_ctx_resize(d->mtl, pp[0], pp[1], !pp[8]);
    d3d_mode_device(d, !pp[8], pp[0], pp[1], 0);
    RET(D3D_OK, 2);
}

static void dv_TestCooperativeLevel(void) { (void)d3d_dev(A32(0)); RET(D3D_OK, 1); }
static void dv_GetAvailableTextureMem(void) { (void)d3d_dev(A32(0)); RET(BW2_VIDMEM, 1); }
static void dv_EvictManagedResources(void) { (void)d3d_dev(A32(0)); RET(D3D_OK, 1); }
static void dv_GetDirect3D(void) {         /* (this, IDirect3D9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(d->d3d9);
    MEM32(A32(1)) = d->d3d9;
    RET(D3D_OK, 2);
}
static void dv_GetDeviceCaps(void) {       /* (this, D3DCAPS9*) */
    (void)d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_fill_caps(A32(1));
    RET(D3D_OK, 2);
}
static void put_display_mode(uint32_t m) {
    uint32_t w, h;
    d3d_current_mode(&w, &h);
    MEM32(m) = w, MEM32(m + 4) = h, MEM32(m + 8) = 60, MEM32(m + 12) = D3DFMT_X8R8G8B8;
}
static void dv_GetDisplayMode(void) {      /* (this, swapchain, D3DDISPLAYMODE*) */
    (void)d3d_dev(A32(0));
    if (A32(1) != 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    put_display_mode(A32(2));
    RET(D3D_OK, 3);
}
static void dv_GetCreationParameters(void) {   /* (this, D3DDEVICE_CREATION_PARAMETERS*) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(D3DERR_INVALIDCALL, 2);
    MEM32(p) = d->adapter, MEM32(p + 4) = d->devtype, MEM32(p + 8) = d->focus, MEM32(p + 12) = d->flags;
    RET(D3D_OK, 2);
}
static void dv_SetCursorProperties(void) { (void)d3d_dev(A32(0)); RET(D3D_OK, 4); }   /* the input plan draws it */
static void dv_SetCursorPosition(void) {   /* (this, x, y, flags): void */
    struct d3d_device *d = d3d_dev(A32(0));
    d->cursor_x = A32(1), d->cursor_y = A32(2);
    RET(0, 4);
}
static void dv_ShowCursor(void) {          /* (this, show): the previous state */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t was = d->cursor_shown;
    d->cursor_shown = A32(1) != 0;
    RET(was, 2);
}
static void dv_GetSwapChain(void) {        /* (this, index, IDirect3DSwapChain9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) != 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    d3d_addref(d->swapchain);
    MEM32(A32(2)) = d->swapchain;
    RET(D3D_OK, 3);
}
static void dv_GetNumberOfSwapChains(void) { (void)d3d_dev(A32(0)); RET(1, 1); }
static void put_raster(uint32_t p) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t w, h;
    d3d_display_size(&w, &h);
    double frame = fmod((double)ts.tv_sec * 60.0 + ts.tv_nsec * 60e-9, 1.0);
    MEM32(p) = 0, MEM32(p + 4) = (uint32_t)(frame * h);
}
static void dv_GetRasterStatus(void) {     /* (this, swapchain, D3DRASTER_STATUS*) */
    (void)d3d_dev(A32(0));
    if (A32(1) != 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    put_raster(A32(2));
    RET(D3D_OK, 3);
}
static void dv_SetDialogBoxMode(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    d->dialog_mode = A32(1) != 0;
    RET(D3D_OK, 2);
}
static void dv_SetGammaRamp(void) {        /* (this, swapchain, flags, D3DGAMMARAMP*): void */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(3)) memcpy(d->gamma, APTR(3), sizeof d->gamma);
    RET(0, 4);
}
static void dv_GetGammaRamp(void) {        /* (this, swapchain, D3DGAMMARAMP*): void */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(2)) memcpy(APTR(2), d->gamma, sizeof d->gamma);
    RET(0, 3);
}
static void dv_BeginScene(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (d->in_scene) RET(D3DERR_INVALIDCALL, 1);
    d->in_scene = 1;
    RET(D3D_OK, 1);
}
static void dv_EndScene(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!d->in_scene) RET(D3DERR_INVALIDCALL, 1);
    d->in_scene = 0;
    RET(D3D_OK, 1);
}

/* ---- IDirect3DSwapChain9 ---- */
static struct d3d_swapchain *swc(uint32_t o) { return d3d_host(o, D3DK_SWAPCHAIN); }
static void sc_Present(void) {             /* (this, src, dst, hwnd, dirty, flags) */
    struct d3d_swapchain *sc = swc(A32(0));
    struct d3d_device *d = d3d_dev(sc->device);
    present_bb(d, sc->backbuffer ? sc->backbuffer : d->backbuffer);
    RET(D3D_OK, 6);
}
static void sc_GetBackBuffer(void) {       /* (this, index, type, IDirect3DSurface9**) */
    struct d3d_swapchain *sc = swc(A32(0));
    struct d3d_device *d = d3d_dev(sc->device);
    uint32_t bb = sc->backbuffer ? sc->backbuffer : d->backbuffer;
    if (A32(1) >= (sc->backbuffer ? sc->pp[3] : d->pp[3]) || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    d3d_addref(bb);
    MEM32(A32(3)) = bb;
    RET(D3D_OK, 4);
}
/* The front buffer: the back buffer Present showed last, into an A8R8G8B8 system-memory surface. */
static uint32_t front_buffer(struct d3d_device *d, uint32_t dst) {
    struct d3d_surface *t = d3d_surf(dst);
    if (t->pool != D3DPOOL_SYSTEMMEM || t->fmt != D3DFMT_A8R8G8B8 || !d->last_presented) return D3DERR_INVALIDCALL;
    struct d3d_surface *s = d3d_surf(d->last_presented);
    if (t->w < s->w || t->h < s->h || s->f->conv != CV_NONE || s->f->bytes != 4) return D3DERR_INVALIDCALL;
    uint32_t sp = s->w * 4;
    uint8_t *tmp = malloc((size_t)sp * s->h), *out = hle_ptr(d3d_surface_mem(t));
    mtl_tex_download(d->mtl, s->tex, s->slice, s->level, 0, 0, s->w, s->h, tmp, sp);
    for (uint32_t y = 0; y < s->h; y++) {
        memcpy(out + y * t->pitch, tmp + y * sp, sp);
        for (uint32_t x = 0; x < s->w; x++) out[y * t->pitch + 4 * x + 3] = 0xFF;
    }
    free(tmp);
    return D3D_OK;
}
static void sc_GetFrontBufferData(void) {  /* (this, surface) */
    struct d3d_swapchain *sc = swc(A32(0));
    RET(front_buffer(d3d_dev(sc->device), A32(1)), 2);
}
static void dv_GetFrontBufferData(void) {  /* (this, swapchain, surface) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) != 0) RET(D3DERR_INVALIDCALL, 3);
    RET(front_buffer(d, A32(2)), 3);
}
static void dv_CreateAdditionalSwapChain(void) {   /* (this, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t pp[14];
    if (!A32(1) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    uint32_t hr = check_pp(d->focus, A32(1), pp);
    if (hr) RET(hr, 3);
    struct d3d_swapchain *sc = calloc(1, sizeof *sc);
    sc->base.destroy = destroy_swapchain, sc->device = d->base.guest;
    memcpy(sc->pp, pp, sizeof pp);
    sc->backbuffer = d3d_surface_new(d->base.guest, 0, pp[2], D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, pp[0], pp[1],
                                     NULL, 0, 0, 0);
    d->default_pool_live++;                              /* released before Reset, like a DEFAULT resource */
    d3d_addref(d->base.guest);
    MEM32(A32(2)) = d3d_new(D3DK_SWAPCHAIN, "IDirect3DSwapChain9", &sc->base);
    RET(D3D_OK, 3);
}
static void dv_SetSoftwareVertexProcessing(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    d->st.sw_vp = A32(1) != 0;
    RET(D3D_OK, 2);
}
static void dv_GetSoftwareVertexProcessing(void) { RET(d3d_dev(A32(0))->st.sw_vp, 1); }
static void dv_SetNPatchMode(void) {       /* (this, float segments) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) != 0) d3d_fail("N-patches (SetNPatchMode 0x%08X) are not supported", A32(1));
    d3d_ws(d, SBW_NPATCH, 0, 0)->npatch_mode = 0;
    RET(D3D_OK, 2);
}
static void dv_GetNPatchMode(void) { RET(d3d_dev(A32(0))->st.npatch_mode, 1); }
static void dv_SetClipStatus(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(d->clip_status, APTR(1), 8);
    RET(D3D_OK, 2);
}
static void dv_GetClipStatus(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), d->clip_status, 8);
    RET(D3D_OK, 2);
}
static void dv_SetPaletteEntries(void) {   /* (this, number, PALETTEENTRY[256]) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t n = A32(1);
    if (!A32(2) || n > 0xFFFF) RET(D3DERR_INVALIDCALL, 3);
    if (n >= d->npalettes) {
        uint32_t k = n + 1;
        d->palettes = realloc(d->palettes, sizeof *d->palettes * k);
        memset(d->palettes + d->npalettes, 0, sizeof *d->palettes * (k - d->npalettes));
        d->npalettes = k;
    }
    memcpy(d->palettes[n], APTR(2), 1024);
    RET(D3D_OK, 3);
}
static void dv_GetPaletteEntries(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(2) || A32(1) >= d->npalettes) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), d->palettes[A32(1)], 1024);
    RET(D3D_OK, 3);
}
static void dv_SetCurrentTexturePalette(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) >= d->npalettes) RET(D3DERR_INVALIDCALL, 2);
    d->st.palette_cur = A32(1);
    RET(D3D_OK, 2);
}
static void dv_GetCurrentTexturePalette(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->st.palette_cur;
    RET(D3D_OK, 2);
}
static void sc_GetRasterStatus(void) {
    (void)swc(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    put_raster(A32(1));
    RET(D3D_OK, 2);
}
static void sc_GetDisplayMode(void) {
    (void)swc(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    put_display_mode(A32(1));
    RET(D3D_OK, 2);
}
static void sc_GetDevice(void) {
    struct d3d_swapchain *s = swc(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(s->device);
    MEM32(A32(1)) = s->device;
    RET(D3D_OK, 2);
}
static void sc_GetPresentParameters(void) {
    struct d3d_swapchain *sc = swc(A32(0));
    struct d3d_device *d = d3d_dev(sc->device);
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), sc->backbuffer ? sc->pp : d->pp, 56);
    RET(D3D_OK, 2);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3D9::CreateDevice", a_CreateDevice },
    { "IDirect3DDevice9::TestCooperativeLevel", dv_TestCooperativeLevel },
    { "IDirect3DDevice9::GetAvailableTextureMem", dv_GetAvailableTextureMem },
    { "IDirect3DDevice9::EvictManagedResources", dv_EvictManagedResources },
    { "IDirect3DDevice9::GetDirect3D", dv_GetDirect3D },
    { "IDirect3DDevice9::GetDeviceCaps", dv_GetDeviceCaps },
    { "IDirect3DDevice9::GetDisplayMode", dv_GetDisplayMode },
    { "IDirect3DDevice9::GetCreationParameters", dv_GetCreationParameters },
    { "IDirect3DDevice9::SetCursorProperties", dv_SetCursorProperties },
    { "IDirect3DDevice9::SetCursorPosition", dv_SetCursorPosition },
    { "IDirect3DDevice9::ShowCursor", dv_ShowCursor },
    { "IDirect3DDevice9::GetSwapChain", dv_GetSwapChain },
    { "IDirect3DDevice9::GetNumberOfSwapChains", dv_GetNumberOfSwapChains },
    { "IDirect3DDevice9::Reset", dv_Reset },
    { "IDirect3DDevice9::Present", dv_Present },
    { "IDirect3DDevice9::GetBackBuffer", dv_GetBackBuffer },
    { "IDirect3DDevice9::GetRasterStatus", dv_GetRasterStatus },
    { "IDirect3DDevice9::SetDialogBoxMode", dv_SetDialogBoxMode },
    { "IDirect3DDevice9::SetGammaRamp", dv_SetGammaRamp },
    { "IDirect3DDevice9::GetGammaRamp", dv_GetGammaRamp },
    { "IDirect3DDevice9::CreateRenderTarget", dv_CreateRenderTarget },
    { "IDirect3DDevice9::CreateDepthStencilSurface", dv_CreateDepthStencilSurface },
    { "IDirect3DDevice9::GetRenderTargetData", dv_GetRenderTargetData },
    { "IDirect3DDevice9::CreateOffscreenPlainSurface", dv_CreateOffscreenPlainSurface },
    { "IDirect3DDevice9::SetRenderTarget", dv_SetRenderTarget },
    { "IDirect3DDevice9::GetRenderTarget", dv_GetRenderTarget },
    { "IDirect3DDevice9::SetDepthStencilSurface", dv_SetDepthStencilSurface },
    { "IDirect3DDevice9::GetDepthStencilSurface", dv_GetDepthStencilSurface },
    { "IDirect3DDevice9::BeginScene", dv_BeginScene },
    { "IDirect3DDevice9::EndScene", dv_EndScene },
    { "IDirect3DDevice9::Clear", dv_Clear },
    { "IDirect3DSwapChain9::Present", sc_Present },
    { "IDirect3DSwapChain9::GetBackBuffer", sc_GetBackBuffer },
    { "IDirect3DSwapChain9::GetRasterStatus", sc_GetRasterStatus },
    { "IDirect3DSwapChain9::GetDisplayMode", sc_GetDisplayMode },
    { "IDirect3DSwapChain9::GetDevice", sc_GetDevice },
    { "IDirect3DSwapChain9::GetPresentParameters", sc_GetPresentParameters },
    { "IDirect3DSwapChain9::GetFrontBufferData", sc_GetFrontBufferData },
    { "IDirect3DDevice9::GetFrontBufferData", dv_GetFrontBufferData },
    { "IDirect3DDevice9::CreateAdditionalSwapChain", dv_CreateAdditionalSwapChain },
    { "IDirect3DDevice9::SetSoftwareVertexProcessing", dv_SetSoftwareVertexProcessing },
    { "IDirect3DDevice9::GetSoftwareVertexProcessing", dv_GetSoftwareVertexProcessing },
    { "IDirect3DDevice9::SetNPatchMode", dv_SetNPatchMode },
    { "IDirect3DDevice9::GetNPatchMode", dv_GetNPatchMode },
    { "IDirect3DDevice9::SetClipStatus", dv_SetClipStatus },
    { "IDirect3DDevice9::GetClipStatus", dv_GetClipStatus },
    { "IDirect3DDevice9::SetPaletteEntries", dv_SetPaletteEntries },
    { "IDirect3DDevice9::GetPaletteEntries", dv_GetPaletteEntries },
    { "IDirect3DDevice9::SetCurrentTexturePalette", dv_SetCurrentTexturePalette },
    { "IDirect3DDevice9::GetCurrentTexturePalette", dv_GetCurrentTexturePalette },
    { 0, 0 } };
void d3d9_device_register(void) { d3d_impls(g_impls); }
