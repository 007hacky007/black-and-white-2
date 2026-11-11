/*
 * d3d9_stateblock.c - IDirect3DStateBlock9 (plan 4 Task 7): CreateStateBlock
 * of the documented state sets (ALL, PIXELSTATE, VERTEXSTATE), BeginStateBlock
 * and EndStateBlock recording only what was set, Capture and Apply.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

struct d3d_stateblock {
    struct d3d_obj base;
    uint32_t device;
    struct d3d_state st;
    struct d3d_sbmask m;
};
static struct d3d_stateblock *sb_of(uint32_t o) { return d3d_host(o, D3DK_STATEBLOCK); }

static void mark_light(struct d3d_sbmask *m, uint32_t i) {
    for (uint32_t k = 0; k < m->nlight; k++) if (m->light[k] == i) return;
    if (m->nlight == m->caplight) {
        uint32_t cap = m->caplight ? 2 * m->caplight : 8;
        uint32_t *l = realloc(m->light, sizeof *l * cap);
        if (!l) d3d_fail("no memory for a state block's lights");
        m->light = l, m->caplight = cap;
    }
    m->light[m->nlight++] = i;
}

struct d3d_state *d3d_ws(struct d3d_device *d, enum sbw what, uint32_t i, uint32_t j) {
    if (!d->recording) return &d->st;
    struct d3d_stateblock *b = sb_of(d->recording);
    struct d3d_sbmask *m = &b->m;
    switch (what) {
    case SBW_RS: m->rs[i] = 1; break;
    case SBW_SS: m->ss[i][j] = 1; break;
    case SBW_TSS: m->tss[i][j] = 1; break;
    case SBW_XFORM: m->xform[i] = 1; break;
    case SBW_CLIP: m->clip[i] = 1; break;
    case SBW_TEX: m->tex[i] = 1; break;
    case SBW_STREAM: m->stream[i] = 1; break;
    case SBW_FREQ: m->freq[i] = 1; break;
    case SBW_MATERIAL: m->material = 1; break;
    case SBW_VP: m->vp = 1; break;
    case SBW_SCISSOR: m->scissor = 1; break;
    case SBW_INDICES: m->indices = 1; break;
    case SBW_VDECL: m->vdecl = 1; break;
    case SBW_VS: m->vs = 1; break;
    case SBW_PS: m->ps = 1; break;
    case SBW_NPATCH: m->npatch = 1; break;
    case SBW_VSF: memset(m->vsf + i, 1, j); break;
    case SBW_PSF: memset(m->psf + i, 1, j); break;
    case SBW_VSI: memset(m->vsi + i, 1, j); break;
    case SBW_PSI: memset(m->psi + i, 1, j); break;
    case SBW_VSB: memset(m->vsb + i, 1, j); break;
    case SBW_PSB: memset(m->psb + i, 1, j); break;
    case SBW_LIGHT: mark_light(m, i); break;
    }
    return &b->st;
}

/* Copy what m names from src to dst; bindings keep their references right. */
static void copy_state(struct d3d_state *dst, const struct d3d_state *src, struct d3d_sbmask *m) {
    for (int i = 0; i < NRS; i++) if (m->rs[i]) dst->rs[i] = src->rs[i];
    for (int s = 0; s < NSMP; s++) for (int t = 0; t < NSS; t++) if (m->ss[s][t]) dst->ss[s][t] = src->ss[s][t];
    for (int s = 0; s < NTS; s++) for (int t = 0; t < NTSS; t++) if (m->tss[s][t]) dst->tss[s][t] = src->tss[s][t];
    for (int i = 0; i < 512; i++) if (m->xform[i]) memcpy(dst->xform[i], src->xform[i], 64);
    for (int i = 0; i < 6; i++) if (m->clip[i]) memcpy(dst->clip[i], src->clip[i], 16);
    for (int i = 0; i < NSMP; i++) if (m->tex[i]) d3d_bind(&dst->tex[i], src->tex[i]);
    for (int i = 0; i < 16; i++)
        if (m->stream[i]) {
            d3d_bind(&dst->stream[i], src->stream[i]);
            dst->stream_off[i] = src->stream_off[i], dst->stream_stride[i] = src->stream_stride[i];
        }
    for (int i = 0; i < 16; i++) if (m->freq[i]) dst->stream_freq[i] = src->stream_freq[i];
    if (m->material) memcpy(dst->material, src->material, sizeof dst->material);
    if (m->vp) dst->vp = src->vp;
    if (m->scissor) dst->scissor = src->scissor;
    if (m->indices) d3d_bind(&dst->indices, src->indices);
    if (m->vdecl) d3d_bind(&dst->vdecl, src->vdecl), dst->fvf = src->fvf;
    if (m->vs) d3d_bind(&dst->vs, src->vs);
    if (m->ps) d3d_bind(&dst->ps, src->ps);
    if (m->npatch) dst->npatch_mode = src->npatch_mode;
    for (int i = 0; i < 256; i++) if (m->vsf[i]) memcpy(dst->vsf[i], src->vsf[i], 16);
    for (int i = 0; i < 224; i++) if (m->psf[i]) memcpy(dst->psf[i], src->psf[i], 16);
    for (int i = 0; i < 16; i++) {
        if (m->vsi[i]) memcpy(dst->vsi[i], src->vsi[i], 16);
        if (m->psi[i]) memcpy(dst->psi[i], src->psi[i], 16);
        if (m->vsb[i]) dst->vsb[i] = src->vsb[i];
        if (m->psb[i]) dst->psb[i] = src->psb[i];
    }
    if (m->all_lights)                                      /* every light the source has */
        for (uint32_t i = 0; i < src->nlights; i++) if (src->lights[i].set) mark_light(m, src->lights[i].index);
    for (uint32_t k = 0; k < m->nlight; k++) {
        const struct d3d_light *l = d3d_light_at((struct d3d_state *)src, m->light[k], 0);
        if (l) *d3d_light_at(dst, m->light[k], 1) = *l;
    }
    dst->dirty = DIRTY_ALL;
}

static void destroy_sb(struct d3d_obj *o) {
    struct d3d_stateblock *b = (struct d3d_stateblock *)o;
    for (int i = 0; i < NSMP; i++) d3d_bind(&b->st.tex[i], 0);
    for (int i = 0; i < 16; i++) d3d_bind(&b->st.stream[i], 0);
    d3d_bind(&b->st.indices, 0), d3d_bind(&b->st.vdecl, 0), d3d_bind(&b->st.vs, 0), d3d_bind(&b->st.ps, 0);
    if (d3d_alive(b->device)) {
        struct d3d_device *d = d3d_host(b->device, D3DK_DEVICE);
        if (d->recording == b->base.guest) d->recording = 0;
    }
    d3d_state_free(&b->st);
    free(b->m.light);
    uint32_t dev = b->device;
    free(b);
    if (d3d_alive(dev)) d3d_release(dev);
}

static uint32_t sb_new(struct d3d_device *d) {
    struct d3d_stateblock *b = calloc(1, sizeof *b);
    b->base.destroy = destroy_sb, b->device = d->base.guest;
    d3d_addref(d->base.guest);
    return d3d_new(D3DK_STATEBLOCK, "IDirect3DStateBlock9", &b->base);
}

/* The documented sets: D3DSBT_PIXELSTATE's and D3DSBT_VERTEXSTATE's render states. */
static const uint16_t g_pixel_rs[] = { 7, 8, 9, 14, 15, 16, 19, 20, 23, 24, 25, 26, 36, 37, 38, 27, 195, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 198, 199, 200, 201, 202, 203, 204, 205, 168, 190, 191,
    192, 171, 174, 175, 176, 185, 186, 187, 188, 189, 193, 194, 206, 207, 208, 209, 0 };
static const uint16_t g_vertex_rs[] = { 22, 28, 34, 35, 36, 37, 38, 48, 139, 141, 140, 136, 137, 142, 148, 147, 145,
    146, 151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 166, 167, 170, 172, 173, 178, 179, 180, 181, 182,
    183, 184, 143, 29, 9, 0 };

static void set_mask(struct d3d_sbmask *m, uint32_t type) {
    memset(m, 0, sizeof *m);
    if (type == 1) {                                         /* ALL */
        memset(m, 1, offsetof(struct d3d_sbmask, light));
        m->light = NULL, m->nlight = m->caplight = 0;
        return;
    }
    if (type == 2) {                                         /* PIXELSTATE */
        for (int i = 0; g_pixel_rs[i]; i++) m->rs[g_pixel_rs[i]] = 1;
        for (int s = 0; s < NTS; s++)
            for (int t = 1; t < NTSS; t++) m->tss[s][t] = t != 11 && t != 24;
        for (int s = 0; s < NSMP; s++) for (int t = 1; t < NSS; t++) m->ss[s][t] = t != 13;
        m->ps = 1;
        memset(m->psf, 1, sizeof m->psf), memset(m->psi, 1, sizeof m->psi), memset(m->psb, 1, sizeof m->psb);
        return;
    }
    for (int i = 0; g_vertex_rs[i]; i++) m->rs[g_vertex_rs[i]] = 1;   /* VERTEXSTATE */
    for (int s = 0; s < NTS; s++) m->tss[s][11] = m->tss[s][24] = 1;
    for (int s = 0; s < NSMP; s++) m->ss[s][13] = 1;
    m->all_lights = 1, m->vs = 1, m->vdecl = 1, m->npatch = 1;
    memset(m->freq, 1, sizeof m->freq);
    memset(m->vsf, 1, sizeof m->vsf), memset(m->vsi, 1, sizeof m->vsi), memset(m->vsb, 1, sizeof m->vsb);
}

static void dv_CreateStateBlock(void) {    /* (this, type, IDirect3DStateBlock9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t type = A32(1);
    if (type < 1 || type > 3 || !A32(2) || d->recording) RET(D3DERR_INVALIDCALL, 3);
    uint32_t o = sb_new(d);
    struct d3d_stateblock *b = sb_of(o);
    set_mask(&b->m, type);
    copy_state(&b->st, &d->st, &b->m);
    MEM32(A32(2)) = o;
    RET(D3D_OK, 3);
}
static void dv_BeginStateBlock(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (d->recording) RET(D3DERR_INVALIDCALL, 1);
    d->recording = sb_new(d);
    RET(D3D_OK, 1);
}
static void dv_EndStateBlock(void) {       /* (this, IDirect3DStateBlock9**) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!d->recording || !A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->recording;
    d->recording = 0;
    RET(D3D_OK, 2);
}
static void sb_Capture(void) {
    struct d3d_stateblock *b = sb_of(A32(0));
    struct d3d_device *d = d3d_dev(b->device);
    copy_state(&b->st, &d->st, &b->m);
    RET(D3D_OK, 1);
}
static void sb_Apply(void) {
    struct d3d_stateblock *b = sb_of(A32(0));
    struct d3d_device *d = d3d_dev(b->device);
    copy_state(&d->st, &b->st, &b->m);
    RET(D3D_OK, 1);
}
static void sb_GetDevice(void) {
    struct d3d_stateblock *b = sb_of(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(b->device);
    MEM32(A32(1)) = b->device;
    RET(D3D_OK, 2);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::CreateStateBlock", dv_CreateStateBlock },
    { "IDirect3DDevice9::BeginStateBlock", dv_BeginStateBlock },
    { "IDirect3DDevice9::EndStateBlock", dv_EndStateBlock },
    { "IDirect3DStateBlock9::Capture", sb_Capture },
    { "IDirect3DStateBlock9::Apply", sb_Apply },
    { "IDirect3DStateBlock9::GetDevice", sb_GetDevice },
    { 0, 0 } };
void d3d9_stateblock_register(void) { d3d_impls(g_impls); }
