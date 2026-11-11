/*
 * d3d9_state.c - render, sampler and texture stage states with the D3D9
 * documented defaults, the viewport and the scissor rect (plan 4 Task 4).
 * A Set of the value already there changes nothing (spec 7.3).
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

static const uint32_t ONE_F = 0x3F800000u;

void d3d_state_defaults(struct d3d_state *st, int auto_ds, uint32_t rt_w, uint32_t rt_h) {
    struct d3d_light *lights = st->lights;
    uint32_t nl = st->nlights;
    uint32_t keep_tex[NSMP], keep_stream[16], keep_ind = st->indices, keep_vdecl = st->vdecl;
    memcpy(keep_tex, st->tex, sizeof keep_tex);
    memcpy(keep_stream, st->stream, sizeof keep_stream);
    memset(st, 0, sizeof *st);
    st->lights = lights, st->nlights = nl;
    for (uint32_t i = 0; i < nl; i++) memset(&lights[i], 0, sizeof lights[i]);
    memcpy(st->tex, keep_tex, sizeof keep_tex);           /* bindings are references: d3d_bind clears them */
    memcpy(st->stream, keep_stream, sizeof keep_stream);
    st->indices = keep_ind, st->vdecl = keep_vdecl;
    uint32_t *rs = st->rs;
    rs[7] = auto_ds ? 1 : 0;                               /* ZENABLE */
    rs[8] = 3;                                             /* FILLMODE SOLID */
    rs[9] = 2;                                             /* SHADEMODE GOURAUD */
    rs[14] = 1;                                            /* ZWRITEENABLE */
    rs[16] = 1;                                            /* LASTPIXEL */
    rs[19] = 2, rs[20] = 1;                                /* SRCBLEND ONE, DESTBLEND ZERO */
    rs[22] = 3;                                            /* CULLMODE CCW */
    rs[23] = 4;                                            /* ZFUNC LESSEQUAL */
    rs[25] = 8;                                            /* ALPHAFUNC ALWAYS */
    rs[37] = ONE_F, rs[38] = ONE_F;                        /* FOGEND, FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1, rs[56] = 8;              /* stencil ops KEEP, func ALWAYS */
    rs[58] = rs[59] = 0xFFFFFFFFu;                         /* STENCILMASK, STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;                                  /* TEXTUREFACTOR */
    rs[136] = 1, rs[137] = 1;                              /* CLIPPING, LIGHTING */
    rs[141] = 1, rs[142] = 1;                              /* COLORVERTEX, LOCALVIEWER */
    rs[145] = 1, rs[146] = 2;                              /* DIFFUSE COLOR1, SPECULAR COLOR2 */
    rs[154] = rs[155] = ONE_F;                             /* POINTSIZE, POINTSIZE_MIN */
    rs[158] = ONE_F;                                       /* POINTSCALE_A */
    rs[161] = 1, rs[162] = 0xFFFFFFFFu;                    /* MULTISAMPLEANTIALIAS, MULTISAMPLEMASK */
    rs[166] = 0x42800000u;                                 /* POINTSIZE_MAX 64 */
    rs[168] = 0xF;                                         /* COLORWRITEENABLE */
    rs[171] = 1;                                           /* BLENDOP ADD */
    rs[172] = 3, rs[173] = 1;                              /* POSITIONDEGREE CUBIC, NORMALDEGREE LINEAR */
    rs[178] = rs[179] = ONE_F;                             /* MIN/MAXTESSELLATIONLEVEL */
    rs[182] = ONE_F;                                       /* ADAPTIVETESS_Z */
    rs[186] = rs[187] = rs[188] = 1, rs[189] = 8;          /* CCW stencil */
    rs[190] = rs[191] = rs[192] = 0xF;                     /* COLORWRITEENABLE1-3 */
    rs[193] = 0xFFFFFFFFu;                                 /* BLENDFACTOR */
    rs[207] = 2, rs[208] = 1, rs[209] = 1;                 /* SRC/DESTBLENDALPHA, BLENDOPALPHA */
    for (int s = 0; s < NSMP; s++) {
        uint32_t *ss = st->ss[s];
        ss[1] = ss[2] = ss[3] = 1;                         /* ADDRESSU/V/W WRAP */
        ss[5] = ss[6] = 1;                                 /* MAG/MINFILTER POINT */
        ss[10] = 1;                                        /* MAXANISOTROPY */
    }
    for (int t = 0; t < NTS; t++) {
        uint32_t *ts = st->tss[t];
        ts[1] = t ? 1 : 4;                                 /* COLOROP: MODULATE on stage 0, DISABLE after */
        ts[2] = 2, ts[3] = 1;                              /* COLORARG1 TEXTURE, COLORARG2 CURRENT */
        ts[4] = t ? 1 : 2;                                 /* ALPHAOP: SELECTARG1 on stage 0 */
        ts[5] = 2, ts[6] = 1;
        ts[11] = (uint32_t)t;                              /* TEXCOORDINDEX */
        ts[26] = ts[27] = ts[28] = 1;                      /* COLORARG0, ALPHAARG0, RESULTARG CURRENT */
    }
    for (int i = 0; i < 512; i++)
        for (int k = 0; k < 4; k++) st->xform[i][k * 5] = 1.0f;
    for (int i = 0; i < 16; i++) st->stream_freq[i] = 1;
    st->vp = (struct d3d_viewport){ 0, 0, rt_w, rt_h, 0.0f, 1.0f };
    st->scissor = (struct d3d_rect){ 0, 0, (int32_t)rt_w, (int32_t)rt_h };
    st->dirty = DIRTY_ALL;
}

void d3d_state_free(struct d3d_state *st) { free(st->lights); st->lights = NULL; st->nlights = 0; }

/* Which group a render state belongs to, for dirty tracking. */
static uint32_t rs_group(uint32_t s) {
    switch (s) {
    case 19: case 20: case 27: case 171: case 168: case 190: case 191: case 192: case 193: case 206: case 207:
    case 208: case 209: case 194: return DIRTY_BLEND;
    case 7: case 14: case 23: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 185:
    case 186: case 187: case 188: case 189: return DIRTY_DS;
    case 8: case 22: case 174: case 175: case 195: case 152: return DIRTY_RASTER;
    default: return DIRTY_STAGE | DIRTY_LIGHT;            /* the fixed-function key reads the rest */
    }
}

static int smp_index(uint32_t s) {
    if (s < 16) return (int)s;
    if (s >= 257 && s <= 260) return 16 + (int)(s - 257);
    return -1;
}

static void s_SetRenderState(void) {                   /* (this, state, value) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t s = A32(1), v = A32(2);
    if (s >= NRS) RET(D3DERR_INVALIDCALL, 3);
    struct d3d_state *w = d3d_ws(d, SBW_RS, s, 0);
    if (w->rs[s] != v) w->rs[s] = v, w->dirty |= rs_group(s);
    RET(D3D_OK, 3);
}
static void s_GetRenderState(void) {                   /* (this, state, DWORD*) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t s = A32(1);
    if (s >= NRS || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d->st.rs[s];
    RET(D3D_OK, 3);
}
static void s_SetSamplerState(void) {                  /* (this, sampler, type, value) */
    struct d3d_device *d = d3d_dev(A32(0));
    int i = smp_index(A32(1));
    uint32_t t = A32(2), v = A32(3);
    if (i < 0 || t == 0 || t >= NSS) RET(D3DERR_INVALIDCALL, 4);
    struct d3d_state *w = d3d_ws(d, SBW_SS, (uint32_t)i, t);
    if (w->ss[i][t] != v) w->ss[i][t] = v, w->dirty |= DIRTY_SAMPLER | DIRTY_STAGE;
    RET(D3D_OK, 4);
}
static void s_GetSamplerState(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    int i = smp_index(A32(1));
    uint32_t t = A32(2);
    if (i < 0 || t == 0 || t >= NSS || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    MEM32(A32(3)) = d->st.ss[i][t];
    RET(D3D_OK, 4);
}
static void s_SetTextureStageState(void) {             /* (this, stage, type, value) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t i = A32(1), t = A32(2), v = A32(3);
    if (i >= NTS || t == 0 || t >= NTSS) RET(D3DERR_INVALIDCALL, 4);
    struct d3d_state *w = d3d_ws(d, SBW_TSS, i, t);
    if (w->tss[i][t] != v) w->tss[i][t] = v, w->dirty |= DIRTY_STAGE;
    RET(D3D_OK, 4);
}
static void s_GetTextureStageState(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t i = A32(1), t = A32(2);
    if (i >= NTS || t == 0 || t >= NTSS || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    MEM32(A32(3)) = d->st.tss[i][t];
    RET(D3D_OK, 4);
}
static void s_SetViewport(void) {                      /* (this, D3DVIEWPORT9*) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    struct d3d_viewport v;
    memcpy(&v, APTR(1), sizeof v);
    struct d3d_state *w = d3d_ws(d, SBW_VP, 0, 0);
    if (memcmp(&v, &w->vp, sizeof v)) w->vp = v, w->dirty |= DIRTY_VIEW;
    RET(D3D_OK, 2);
}
static void s_GetViewport(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), &d->st.vp, sizeof d->st.vp);
    RET(D3D_OK, 2);
}
static void s_SetScissorRect(void) {                   /* (this, RECT*) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    struct d3d_state *w = d3d_ws(d, SBW_SCISSOR, 0, 0);
    memcpy(&w->scissor, APTR(1), sizeof w->scissor);
    w->dirty |= DIRTY_VIEW;
    RET(D3D_OK, 2);
}
static void s_GetScissorRect(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), &d->st.scissor, sizeof d->st.scissor);
    RET(D3D_OK, 2);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::SetRenderState", s_SetRenderState },
    { "IDirect3DDevice9::GetRenderState", s_GetRenderState },
    { "IDirect3DDevice9::SetSamplerState", s_SetSamplerState },
    { "IDirect3DDevice9::GetSamplerState", s_GetSamplerState },
    { "IDirect3DDevice9::SetTextureStageState", s_SetTextureStageState },
    { "IDirect3DDevice9::GetTextureStageState", s_GetTextureStageState },
    { "IDirect3DDevice9::SetViewport", s_SetViewport },
    { "IDirect3DDevice9::GetViewport", s_GetViewport },
    { "IDirect3DDevice9::SetScissorRect", s_SetScissorRect },
    { "IDirect3DDevice9::GetScissorRect", s_GetScissorRect },
    { 0, 0 } };
void d3d9_state_register(void) { d3d_impls(g_impls); }
