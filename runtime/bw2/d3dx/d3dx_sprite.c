/*
 * d3dx_sprite.c - ID3DXSprite (plan 7 Task 2), after the ID3DXSprite
 * documentation. Draw queues textured quads; Flush and End draw them with the
 * device's own DrawPrimitiveUP, one call per run of equal textures, after
 * setting the states ID3DXSprite::Begin documents. Every device call goes
 * through the device's guest vtable (d3d_call), so state blocks, Get calls
 * and draws see what the game would see.
 *
 * Screen space (no OBJECTSPACE): the corners are transformed on the CPU and
 * drawn as pre-transformed vertices (XYZRHW, rhw 1), moved by -0.5 so texel
 * centres land on D3D9's pixel centres; the device's transforms are left
 * alone. Object space: the corners stay in sprite space and the world
 * transform is the sprite transform. Without DONOTSAVESTATE a D3DSBT_ALL
 * state block is captured at Begin and applied at End.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "d3d_com.h"
#include "bw2.h"
#include "ui.h"

#define KNOWN_FLAGS (D3DXSPRITE_DONOTSAVESTATE | D3DXSPRITE_DONOTMODIFY_RENDERSTATE | D3DXSPRITE_OBJECTSPACE | \
                     D3DXSPRITE_ALPHABLEND | D3DXSPRITE_SORT_TEXTURE | D3DXSPRITE_SORT_DEPTH_FRONTTOBACK | \
                     D3DXSPRITE_SORT_DEPTH_BACKTOFRONT)
#define FVF_SCREEN 0x144u                    /* XYZRHW | DIFFUSE | TEX1 */
#define FVF_OBJECT 0x142u                    /* XYZ | DIFFUSE | TEX1 */
#define D3DTS_WORLD 256u
#define D3DPT_TRIANGLELIST 4u

struct item {
    uint32_t tex, color, seq, rank;
    float z;                                  /* the depth sorted by: the corners' mean, transformed */
    float c[4][6];                            /* TL, TR, BL, BR: x, y, z, rhw, u, v */
    float m[16];                              /* object space: the sprite transform at Draw */
};
struct sprite {
    struct d3d_obj base;
    uint32_t device;
    float xf[16];                             /* D3DXMATRIX, row vectors: v' = v * M */
    int begun;
    uint32_t flags, sb;                       /* the Begin's flags; the D3DSBT_ALL state block (or 0) */
    struct item *q;
    uint32_t nq, capq;
    uint32_t scratch;                         /* 64 guest bytes for out-parameters */
    uint8_t *vb;                              /* guest vertex memory for DrawPrimitiveUP */
    uint32_t vbcap;
};

static uint32_t g_draw_calls;
uint32_t d3dx_sprite_draw_calls(void) { return g_draw_calls; }

static const float g_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static struct sprite *sp_of(uint32_t o) { return d3d_host(o, D3DK_SPRITE); }

/* the device's methods, through its guest vtable; a Set the device refuses stops loudly */
static uint32_t dev(struct sprite *s, const char *m, uint32_t n, ...) {
    uint32_t a[8];
    va_list ap;
    va_start(ap, n);
    for (uint32_t i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return d3d_call(s->device, "IDirect3DDevice9", m, n, a);
}
static void dev_ok(struct sprite *s, const char *m, uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t a[3] = { a1, a2, a3 };
    uint32_t hr = d3d_call(s->device, "IDirect3DDevice9", m, n, a);
    if (hr) d3d_fail("ID3DXSprite: IDirect3DDevice9::%s(0x%X, 0x%X, 0x%X) gave 0x%08X", m, a1, a2, a3, hr);
}
static uint32_t unk(uint32_t o, const char *iface, const char *m) { return d3d_call(o, iface, m, 0, NULL); }

static void drop_queue(struct sprite *s) {
    for (uint32_t i = 0; i < s->nq; i++) unk(s->q[i].tex, "IDirect3DTexture9", "Release");
    s->nq = 0;
}
static void drop_stateblock(struct sprite *s) {
    if (s->sb) unk(s->sb, "IDirect3DStateBlock9", "Release");
    s->sb = 0;
}

/* ---- the states a draw sets (ID3DXSprite::Begin) ---- */

static const uint16_t g_rs[][2] = {
    { 25, 5 },    /* ALPHAFUNC GREATER */
    { 24, 0 },    /* ALPHAREF */
    { 171, 1 },   /* BLENDOP ADD */
    { 19, 5 },    /* SRCBLEND SRCALPHA */
    { 20, 6 },    /* DESTBLEND INVSRCALPHA */
    { 206, 0 },   /* SEPARATEALPHABLENDENABLE */
    { 136, 1 },   /* CLIPPING */
    { 152, 0 },   /* CLIPPLANEENABLE */
    { 168, 0xF }, /* COLORWRITEENABLE */
    { 22, 1 },    /* CULLMODE NONE */
    { 145, 1 },   /* DIFFUSEMATERIALSOURCE COLOR1 */
    { 8, 3 },     /* FILLMODE SOLID */
    { 28, 0 },    /* FOGENABLE */
    { 48, 0 },    /* RANGEFOGENABLE */
    { 167, 0 },   /* INDEXEDVERTEXBLENDENABLE */
    { 151, 0 },   /* VERTEXBLEND */
    { 137, 0 },   /* LIGHTING */
    { 9, 2 },     /* SHADEMODE GOURAUD */
    { 29, 0 },    /* SPECULARENABLE */
    { 194, 0 },   /* SRGBWRITEENABLE */
    { 52, 0 },    /* STENCILENABLE */
    { 128, 0 },   /* WRAP0 */
};
static const uint8_t g_tss[][3] = {
    { 0, 1, 4 }, { 0, 2, 2 }, { 0, 3, 0 },    /* COLOROP MODULATE, TEXTURE, DIFFUSE */
    { 0, 4, 4 }, { 0, 5, 2 }, { 0, 6, 0 },    /* ALPHAOP MODULATE, TEXTURE, DIFFUSE */
    { 0, 11, 0 }, { 0, 24, 0 },               /* TEXCOORDINDEX 0, TEXTURETRANSFORMFLAGS DISABLE */
    { 1, 1, 1 }, { 1, 4, 1 },                 /* stage 1 COLOROP, ALPHAOP DISABLE */
};
static const uint8_t g_ss[][2] = {
    { 1, 3 }, { 2, 3 },                       /* ADDRESSU, ADDRESSV CLAMP */
    { 5, 2 }, { 6, 2 }, { 7, 2 },             /* MAG, MIN, MIPFILTER LINEAR */
    { 9, 0 }, { 10, 1 }, { 8, 0 }, { 11, 0 }, /* MAXMIPLEVEL, MAXANISOTROPY, MIPMAPLODBIAS, SRGBTEXTURE */
};

static void set_states(struct sprite *s) {
    uint32_t blend = (s->flags & D3DXSPRITE_ALPHABLEND) != 0;
    dev_ok(s, "SetRenderState", 2, 27, blend, 0);              /* ALPHABLENDENABLE */
    dev_ok(s, "SetRenderState", 2, 15, blend, 0);              /* ALPHATESTENABLE */
    for (size_t i = 0; i < sizeof g_rs / sizeof *g_rs; i++) dev_ok(s, "SetRenderState", 2, g_rs[i][0], g_rs[i][1], 0);
    for (size_t i = 0; i < sizeof g_tss / sizeof *g_tss; i++)
        dev_ok(s, "SetTextureStageState", 3, g_tss[i][0], g_tss[i][1], g_tss[i][2]);
    for (size_t i = 0; i < sizeof g_ss / sizeof *g_ss; i++) dev_ok(s, "SetSamplerState", 3, 0, g_ss[i][0], g_ss[i][1]);
    dev_ok(s, "SetVertexShader", 1, 0, 0, 0);
    dev_ok(s, "SetPixelShader", 1, 0, 0, 0);
}

/* ---- drawing the queue ---- */

static uint32_t g_sort_flags;
static int by_order(const void *pa, const void *pb) {
    const struct item *a = *(const struct item *const *)pa, *b = *(const struct item *const *)pb;
    if ((g_sort_flags & D3DXSPRITE_SORT_DEPTH_FRONTTOBACK) && a->z != b->z) return a->z < b->z ? -1 : 1;
    if ((g_sort_flags & D3DXSPRITE_SORT_DEPTH_BACKTOFRONT) && a->z != b->z) return a->z > b->z ? -1 : 1;
    if ((g_sort_flags & D3DXSPRITE_SORT_TEXTURE) && a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;                /* stable */
}

static uint8_t *vertices(struct sprite *s, uint32_t bytes) {
    if (bytes > s->vbcap) {
        if (s->vb) hle_free(s->vb);
        s->vbcap = bytes > 2 * s->vbcap ? bytes : 2 * s->vbcap;
        s->vb = hle_alloc(s->vbcap);
        if (!s->vb) d3d_fail("ID3DXSprite: no guest memory for %u vertex bytes", s->vbcap);
    }
    return s->vb;
}

static void flush(struct sprite *s) {
    if (!s->nq) return;
    int object = (s->flags & D3DXSPRITE_OBJECTSPACE) != 0;
    /* texture ranks by first appearance, for SORT_TEXTURE */
    uint32_t *seen = malloc(sizeof *seen * s->nq), nseen = 0;
    struct item **order = malloc(sizeof *order * s->nq);
    if (!seen || !order) d3d_fail("ID3DXSprite: no memory to sort %u sprites", s->nq);
    for (uint32_t i = 0; i < s->nq; i++) {
        uint32_t r = 0;
        while (r < nseen && seen[r] != s->q[i].tex) r++;
        if (r == nseen) seen[nseen++] = s->q[i].tex;
        s->q[i].rank = r;
        order[i] = &s->q[i];
    }
    g_sort_flags = s->flags;
    qsort(order, s->nq, sizeof *order, by_order);

    if (!(s->flags & D3DXSPRITE_DONOTMODIFY_RENDERSTATE)) set_states(s);
    dev_ok(s, "SetFVF", 1, object ? FVF_OBJECT : FVF_SCREEN, 0, 0);
    uint32_t stride = object ? 24u : 28u;
    for (uint32_t i = 0; i < s->nq;) {
        uint32_t j = i + 1;
        while (j < s->nq && order[j]->tex == order[i]->tex && (!object || !memcmp(order[j]->m, order[i]->m, 64))) j++;
        uint8_t *v = vertices(s, (j - i) * 6 * stride), *p = v;
        for (uint32_t k = i; k < j; k++) {
            static const int corner[6] = { 0, 1, 2, 1, 3, 2 };      /* clockwise on screen */
            for (int c = 0; c < 6; c++) {
                const float *f = order[k]->c[corner[c]];
                float xyzw[4] = { f[0], f[1], f[2], f[3] }, uv[2] = { f[4], f[5] };
                memcpy(p, xyzw, object ? 12 : 16), p += object ? 12 : 16;
                memcpy(p, &order[k]->color, 4), p += 4;
                memcpy(p, uv, 8), p += 8;
            }
        }
        dev_ok(s, "SetTexture", 2, 0, order[i]->tex, 0);
        if (object) {
            memcpy(hle_ptr(s->scratch), order[i]->m, 64);
            dev_ok(s, "SetTransform", 2, D3DTS_WORLD, s->scratch, 0);
        }
        uint32_t hr = dev(s, "DrawPrimitiveUP", 4, D3DPT_TRIANGLELIST, (j - i) * 2, hle_va(v), stride);
        if (hr) d3d_fail("ID3DXSprite: DrawPrimitiveUP of %u sprites gave 0x%08X", j - i, hr);
        g_draw_calls++;
        i = j;
    }
    free(order);
    free(seen);
    drop_queue(s);
}

/* ---- methods ---- */

static void sp_GetDevice(void) {                 /* (this, IDirect3DDevice9**) */
    struct sprite *s = sp_of(A32(0));
    uint32_t out = A32(1);
    if (!out) RET(D3DERR_INVALIDCALL, 2);
    unk(s->device, "IDirect3DDevice9", "AddRef");
    MEM32(out) = s->device;
    RET(D3D_OK, 2);
}
static void sp_GetTransform(void) {              /* (this, D3DXMATRIX*) */
    struct sprite *s = sp_of(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), s->xf, 64);
    RET(D3D_OK, 2);
}
static void sp_SetTransform(void) {              /* (this, const D3DXMATRIX*) */
    struct sprite *s = sp_of(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(s->xf, APTR(1), 64);
    RET(D3D_OK, 2);
}
static void sp_Begin(void) {                     /* (this, flags) */
    struct sprite *s = sp_of(A32(0));
    uint32_t flags = A32(1);
    if (flags & D3DXSPRITE_BILLBOARD) d3d_fail("ID3DXSprite::Begin: D3DXSPRITE_BILLBOARD (0x8) is not handled");
    if (flags & ~KNOWN_FLAGS) d3d_fail("ID3DXSprite::Begin: flag 0x%X is not handled", flags & ~KNOWN_FLAGS);
    if ((flags & D3DXSPRITE_SORT_DEPTH_FRONTTOBACK) && (flags & D3DXSPRITE_SORT_DEPTH_BACKTOFRONT))
        d3d_fail("ID3DXSprite::Begin: SORT_DEPTH_FRONTTOBACK and SORT_DEPTH_BACKTOFRONT together (flags 0x%X)", flags);
    if (s->begun) RET(D3DERR_INVALIDCALL, 2);
    if (!(flags & D3DXSPRITE_DONOTSAVESTATE)) {
        if (!s->sb) {
            uint32_t hr = dev(s, "CreateStateBlock", 2, 1u, s->scratch);       /* D3DSBT_ALL */
            if (hr) d3d_fail("ID3DXSprite::Begin: CreateStateBlock gave 0x%08X", hr);
            s->sb = MEM32(s->scratch);
        }
        uint32_t hr = unk(s->sb, "IDirect3DStateBlock9", "Capture");
        if (hr) d3d_fail("ID3DXSprite::Begin: Capture gave 0x%08X", hr);
    }
    s->begun = 1, s->flags = flags;
    RET(D3D_OK, 2);
}
static void corner(const struct sprite *s, int object, float x, float y, float z, float *c) {
    if (object) { c[0] = x, c[1] = y, c[2] = z, c[3] = 1; return; }
    const float *m = s->xf;
    float tx = x * m[0] + y * m[4] + z * m[8] + m[12], ty = x * m[1] + y * m[5] + z * m[9] + m[13];
    float tz = x * m[2] + y * m[6] + z * m[10] + m[14], tw = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (tw == 0.0f) d3d_fail("ID3DXSprite::Draw: the sprite transform gives w 0");
    if (tw != 1.0f) tx /= tw, ty /= tw, tz /= tw;
    c[0] = tx - 0.5f, c[1] = ty - 0.5f, c[2] = tz, c[3] = 1.0f / tw;
}
static float depth(const float *m, float x, float y, float z) { return x * m[2] + y * m[6] + z * m[10] + m[14]; }
/* (this, IDirect3DTexture9*, const RECT* src, const D3DXVECTOR3* center, const D3DXVECTOR3* position, D3DCOLOR) */
static void sp_Draw(void) {
    struct sprite *s = sp_of(A32(0));
    uint32_t tex = A32(1), rc = A32(2), cen = A32(3), pos = A32(4), color = A32(5);
    if (!s->begun || !tex) RET(D3DERR_INVALIDCALL, 6);
    d3d_host(tex, D3DK_TEXTURE);
    uint32_t a[2] = { 0, s->scratch };
    uint32_t hr = d3d_call(tex, "IDirect3DTexture9", "GetLevelDesc", 2, a);
    if (hr) d3d_fail("ID3DXSprite::Draw: GetLevelDesc of texture 0x%08X gave 0x%08X", tex, hr);
    float w = (float)MEM32(s->scratch + 24), h = (float)MEM32(s->scratch + 28);
    int32_t l = 0, t = 0, r = (int32_t)w, b = (int32_t)h;
    if (rc) l = (int32_t)MEM32(rc), t = (int32_t)MEM32(rc + 4), r = (int32_t)MEM32(rc + 8), b = (int32_t)MEM32(rc + 12);
    float c3[3] = { 0, 0, 0 }, p3[3] = { 0, 0, 0 };
    if (cen) memcpy(c3, hle_ptr(cen), 12);
    if (pos) memcpy(p3, hle_ptr(pos), 12);
    if (s->nq == s->capq) {
        s->capq = s->capq ? 2 * s->capq : 64;
        s->q = realloc(s->q, sizeof *s->q * s->capq);
        if (!s->q) d3d_fail("ID3DXSprite: no memory for %u sprites", s->capq);
    }
    struct item *it = &s->q[s->nq];
    memset(it, 0, sizeof *it);
    int object = (s->flags & D3DXSPRITE_OBJECTSPACE) != 0;
    float x0 = p3[0] - c3[0], y0 = p3[1] - c3[1], z = p3[2] - c3[2];
    float x1 = x0 + (float)(r - l), y1 = y0 + (float)(b - t);
    float u0 = (float)l / w, u1 = (float)r / w, v0 = (float)t / h, v1 = (float)b / h;
    const float xy[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } }, uv[4][2] = { { u0, v0 }, { u1, v0 },
                             { u0, v1 }, { u1, v1 } };
    it->z = 0;
    for (int k = 0; k < 4; k++) {
        corner(s, object, xy[k][0], xy[k][1], z, it->c[k]);
        it->c[k][4] = uv[k][0], it->c[k][5] = uv[k][1];
        it->z += depth(s->xf, xy[k][0], xy[k][1], z) / 4;
    }
    if (object) memcpy(it->m, s->xf, 64);
    it->tex = tex, it->color = color, it->seq = s->nq;
    s->nq++;
    unk(tex, "IDirect3DTexture9", "AddRef");                   /* held until drawn */
    RET(D3D_OK, 6);
}
static void sp_Flush(void) {
    struct sprite *s = sp_of(A32(0));
    if (!s->begun) RET(D3DERR_INVALIDCALL, 1);
    flush(s);
    RET(D3D_OK, 1);
}
static void sp_End(void) {
    struct sprite *s = sp_of(A32(0));
    if (!s->begun) RET(D3DERR_INVALIDCALL, 1);
    flush(s);
    if (!(s->flags & D3DXSPRITE_DONOTSAVESTATE) && s->sb) {
        uint32_t hr = unk(s->sb, "IDirect3DStateBlock9", "Apply");
        if (hr) d3d_fail("ID3DXSprite::End: Apply gave 0x%08X", hr);
    }
    s->begun = 0;
    RET(D3D_OK, 1);
}
static void sp_OnLostDevice(void) {              /* the batch ends undrawn; the state block goes */
    struct sprite *s = sp_of(A32(0));
    drop_queue(s);
    s->begun = 0;
    drop_stateblock(s);
    RET(D3D_OK, 1);
}
static void sp_OnResetDevice(void) {
    (void)sp_of(A32(0));
    RET(D3D_OK, 1);
}

static void sprite_destroy(struct d3d_obj *o) {
    struct sprite *s = (struct sprite *)o;
    drop_queue(s);
    drop_stateblock(s);
    free(s->q);
    if (s->vb) hle_free(s->vb);
    hle_free(hle_ptr(s->scratch));
    uint32_t device = s->device;
    free(s);
    unk(device, "IDirect3DDevice9", "Release");
}

uint32_t d3dx_sprite_create(uint32_t device) {
    d3d_host(device, D3DK_DEVICE);
    struct sprite *s = calloc(1, sizeof *s);
    if (!s) d3d_fail("ID3DXSprite: no memory");
    s->base.destroy = sprite_destroy;
    s->device = device;
    memcpy(s->xf, g_identity, 64);
    s->scratch = hle_va(hle_alloc(64));
    unk(device, "IDirect3DDevice9", "AddRef");
    return d3d_new(D3DK_SPRITE, "ID3DXSprite", &s->base);
}
static void x_D3DXCreateSprite(void) {           /* (IDirect3DDevice9*, ID3DXSprite**) */
    uint32_t device = A32(0), out = A32(1);
    if (!device || !out) RET(D3DERR_INVALIDCALL, 2);
    MEM32(out) = d3dx_sprite_create(device);
    RET(D3D_OK, 2);
}

static const win32hle_shim g_impls[] = {
    { "ID3DXSprite::GetDevice", sp_GetDevice }, { "ID3DXSprite::GetTransform", sp_GetTransform },
    { "ID3DXSprite::SetTransform", sp_SetTransform }, { "ID3DXSprite::Begin", sp_Begin },
    { "ID3DXSprite::Draw", sp_Draw }, { "ID3DXSprite::Flush", sp_Flush }, { "ID3DXSprite::End", sp_End },
    { "ID3DXSprite::OnLostDevice", sp_OnLostDevice }, { "ID3DXSprite::OnResetDevice", sp_OnResetDevice },
    { 0, 0 } };
static const win32hle_shim g_exports[] = { { "D3DXCreateSprite", x_D3DXCreateSprite }, { 0, 0 } };
void d3dx_sprite_register(void) { d3d_impls(g_impls); win32hle_register(g_exports); }
