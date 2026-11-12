/*
 * d3d9_shader.c - IDirect3DVertexShader9 (plan 5 Task 3) and
 * IDirect3DPixelShader9 (Task 4): the decoded shader, its tokens as given
 * (GetFunction), its def'd constants, and its Metal libraries, one per
 * variant. The first is compiled in the background when the shader is made;
 * a draw that needs another variant starts it. Each shader's part of a draw
 * is built here too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"
#include "ff.h"
#include "sm.h"

struct d3d_def { uint8_t kind; uint16_t reg; uint32_t v[4]; };   /* kind: 0 float, 1 int, 2 bool */
struct d3d_variant { uint8_t v[32]; mtl_obj lib; };      /* an sm_vs_variant or sm_ps_variant, zero-padded */

struct d3d_shader {
    struct d3d_obj base;
    uint32_t device;
    uint32_t *tokens, ntokens;               /* as given, comments included */
    struct sm_shader sm;
    struct d3d_def *defs;
    uint32_t ndefs;
    uint32_t declared;                       /* the input registers with a dcl */
    uint8_t usage[16], usage_index[16];
    uint32_t samplers, projectable;          /* pixel shaders: sm_ps_samplers' */
    struct d3d_variant *var;
    uint32_t nvar;
    char name[48];
};

static void destroy_shader(struct d3d_obj *o) {
    struct d3d_shader *s = (struct d3d_shader *)o;
    uint32_t dev = s->device;
    for (uint32_t i = 0; i < s->nvar; i++) mtl_release(s->var[i].lib);
    free(s->var), free(s->defs), free(s->tokens);
    sm_free(&s->sm);
    free(s);
    if (dev && d3d_alive(dev)) d3d_release(dev);
}

/* The library for a variant (vsize bytes of v): made (and started) on first use. */
static mtl_obj variant_lib(struct d3d_shader *s, const void *v, size_t vsize) {
    uint8_t key[32] = { 0 };
    memcpy(key, v, vsize);
    for (uint32_t i = 0; i < s->nvar; i++)
        if (!memcmp(s->var[i].v, key, sizeof key)) return s->var[i].lib;
    char *msl = s->sm.is_ps ? sm_msl_fragment(&s->sm, v) : sm_msl_vertex(&s->sm, v);
    s->var = realloc(s->var, sizeof *s->var * (s->nvar + 1));
    if (!s->var) abort();
    memcpy(s->var[s->nvar].v, key, sizeof key);
    s->var[s->nvar].lib = mtl_library_async(msl, s->name);
    free(msl);
    return s->var[s->nvar++].lib;
}
_Static_assert(sizeof(struct sm_vs_variant) <= 32 && sizeof(struct sm_ps_variant) <= 32, "variants fit the key");

/* The decoded shader, its tokens and its def'd constants (method: the Create call, for messages). */
static struct d3d_shader *shader_new(const uint32_t *tokens, const char *method, const char *what) {
    struct d3d_shader *s = calloc(1, sizeof *s);
    char err[256];
    if (sm_decode(tokens, 1u << 20, &s->sm, err, sizeof err)) d3d_fail("IDirect3DDevice9::%s: %s", method, err);
    s->ntokens = s->sm.ntokens;
    s->tokens = malloc(4 * (size_t)s->ntokens);
    memcpy(s->tokens, tokens, 4 * (size_t)s->ntokens);
    char ver[16];
    snprintf(s->name, sizeof s->name, "%s %s %016llx", what, sm_version_name(s->sm.version, ver, sizeof ver),
             (unsigned long long)s->sm.hash);
    for (uint32_t i = 0; i < s->sm.nins; i++) {
        const struct sm_ins *in = &s->sm.ins[i];
        if (in->op == SM_DCL && in->dst.type == SM_R_INPUT && in->dst.index < 16) {
            s->declared |= 1u << in->dst.index;
            s->usage[in->dst.index] = (uint8_t)in->usage, s->usage_index[in->dst.index] = (uint8_t)in->usage_index;
        } else if (in->op == SM_DEF || in->op == SM_DEFI || in->op == SM_DEFB) {
            s->defs = realloc(s->defs, sizeof *s->defs * (s->ndefs + 1));
            struct d3d_def *d = &s->defs[s->ndefs++];
            d->kind = in->op == SM_DEF ? 0 : in->op == SM_DEFI ? 1 : 2;
            d->reg = in->dst.index;
            if (in->op == SM_DEF) memcpy(d->v, in->def, 16);
            else memcpy(d->v, in->defi, 16);
        }
    }
    s->base.destroy = destroy_shader;
    return s;
}

uint32_t d3d_vs_create(uint32_t device, const uint32_t *tokens, uint32_t *out) {
    if (!tokens || !out) return D3DERR_INVALIDCALL;
    if (tokens[0] >> 16 != 0xFFFEu) return D3DERR_INVALIDCALL;        /* not a vertex shader */
    struct d3d_shader *s = shader_new(tokens, "CreateVertexShader", "vertex shader");
    /* the usual draw: every input fed by a float element, no clip planes, triangles, Gouraud */
    struct sm_vs_variant v;
    memset(&v, 0, sizeof v);
    for (int r = 0; r < 16; r++) v.input[r] = s->declared >> r & 1 ? SM_IN_FLOAT : SM_IN_NONE;
    variant_lib(s, &v, sizeof v);
    s->device = device;
    if (device) d3d_addref(device);
    *out = d3d_new(D3DK_VSHADER, "IDirect3DVertexShader9", &s->base);
    return D3D_OK;
}

uint32_t d3d_ps_create(uint32_t device, const uint32_t *tokens, uint32_t *out) {
    if (!tokens || !out) return D3DERR_INVALIDCALL;
    if (tokens[0] >> 16 != 0xFFFFu) return D3DERR_INVALIDCALL;        /* not a pixel shader */
    struct d3d_shader *s = shader_new(tokens, "CreatePixelShader", "pixel shader");
    s->samplers = sm_ps_samplers(&s->sm, &s->projectable);
    /* the usual draw: a 2D texture (or the dcl's type) on each sampler read, no fog, no alpha test */
    struct sm_ps_variant v;
    sm_ps_variant_default(&s->sm, &v);
    variant_lib(s, &v, sizeof v);
    s->device = device;
    if (device) d3d_addref(device);
    *out = d3d_new(D3DK_PSHADER, "IDirect3DPixelShader9", &s->base);
    return D3D_OK;
}

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

void d3d_vs_build(struct d3d_device *d, uint32_t vso, const uint8_t (*el)[8], uint32_t nel, uint32_t prim,
                  struct ff_draw *dr, struct ff_vs_u *vu, struct ff_prog *p, struct d3d_vs_consts *k) {
    struct d3d_shader *s = d3d_host(vso, D3DK_VSHADER);
    const uint32_t *rs = d->st.rs;
    struct sm_vs_variant v;
    memset(&v, 0, sizeof v);
    /* each dcl reads the element with its usage and index (the first, if repeated); none reads (0, 0, 0, 1) */
    dr->nattr = 0;
    for (int r = 0; r < 16; r++) {
        if (!(s->declared >> r & 1)) continue;
        uint32_t i = 0;
        while (i < nel && !(el[i][6] == s->usage[r] && el[i][7] == s->usage_index[r] && el[i][4] != 17)) i++;
        if (i == nel) { v.input[r] = SM_IN_NONE; continue; }
        uint32_t stream = el[i][0] | el[i][1] << 8, type = el[i][4];
        if (stream >= 16) d3d_fail("vertex element on stream %u", stream);
        v.input[r] = type == 5 ? SM_IN_UINT : type == 6 || type == 7 ? SM_IN_INT : SM_IN_FLOAT;
        struct ff_attr *a = &dr->attr[dr->nattr++];
        a->slot = (uint8_t)r, a->decltype = (uint8_t)type, a->stream = (uint8_t)stream;
        a->offset = el[i][2] | el[i][3] << 8;
    }
    /* user clip planes: in clip space with a vertex shader */
    uint32_t cp = rs[152] & 0x3F;
    for (int c = 0; c < 6; c++)
        if (cp & (1u << c)) memcpy(vu->clip[v.nclip++], d->st.clip[c], 16);
    v.points = prim == 1;
    v.flat = rs[9] == 1;
    vu->vs_wfog = d->st.xform[3][11] != 0.0f ? 1.0f : 0.0f;
    vu->vs_psize = f32(rs[154]);
    /* the constants, with the shader's own def'd ones over the device's */
    memcpy(k->f, d->st.vsf, sizeof k->f);
    memcpy(k->i, d->st.vsi, sizeof k->i);
    memcpy(k->b, d->st.vsb, sizeof k->b);
    for (uint32_t i = 0; i < s->ndefs; i++) {
        const struct d3d_def *df = &s->defs[i];
        if (df->kind == 0 && df->reg < 256) memcpy(k->f[df->reg], df->v, 16);
        else if (df->kind == 1 && df->reg < 16) memcpy(k->i[df->reg], df->v, 16);
        else if (df->kind == 2 && df->reg < 16) k->b[df->reg] = df->v[0];
    }
    memset(p, 0, sizeof *p);
    p->lib = variant_lib(s, &v, sizeof v);
    p->hash = s->sm.hash;
    memcpy(p->variant, &v, sizeof v);
    p->f = k->f, p->fsize = sizeof k->f;
    p->ib = k->i, p->ibsize = sizeof k->i + sizeof k->b;
}

void d3d_ps_build(struct d3d_device *d, uint32_t pso, const struct ff_pkey *pk, struct ff_draw *dr,
                  struct ff_ps_u *pu, struct ff_prog *p, struct d3d_ps_consts *k) {
    struct d3d_shader *s = d3d_host(pso, D3DK_PSHADER);
    const struct d3d_state *st = &d->st;
    struct sm_ps_variant v;
    memset(&v, 0, sizeof v);
    /* the samplers it reads: the bound textures and their types (ps 1.x samples stage n as sampler n) */
    for (uint32_t i = 0; i < 16; i++) {
        dr->tex[i] = NULL;
        if (s->samplers >> i & 1) v.textype[i] = (uint8_t)ff_bind_sampler(d, i, dr, pu);
    }
    /* ps 1.1-1.3: D3DTTFF_PROJECTED divides the coordinates by the component the count names */
    for (int i = 0; i < 8; i++)
        if (s->projectable >> i & 1 && st->tss[i][24] & 256) {
            uint32_t n = st->tss[i][24] & 0xFF;
            v.proj[i] = (uint8_t)(n >= 1 && n <= 4 ? n : 4);
        }
    v.fog = pk->fog, v.alpha_func = pk->alpha_func, v.flat = pk->flat, v.srgb = pk->srgb_write;
    /* the constants, its def'd ones over the device's; ps 1.x reads them clamped to [-1, 1] */
    memcpy(k->f, st->psf, sizeof k->f);
    for (uint32_t i = 0; i < s->ndefs; i++)
        if (s->defs[i].kind == 0 && s->defs[i].reg < 32) memcpy(k->f[s->defs[i].reg], s->defs[i].v, 16);
    if (s->sm.major == 1)
        for (int i = 0; i < 32; i++)
            for (int j = 0; j < 4; j++) {
                float x = k->f[i][j];
                k->f[i][j] = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
            }
    memset(p, 0, sizeof *p);
    p->lib = variant_lib(s, &v, sizeof v);
    p->hash = s->sm.hash;
    memcpy(p->variant, &v, sizeof v);
    p->f = k->f, p->fsize = sizeof k->f;
}

static void get_device(uint32_t kind) {     /* (this, IDirect3DDevice9**) */
    struct d3d_shader *s = d3d_host(A32(0), kind);
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(s->device);
    MEM32(A32(1)) = s->device;
    RET(D3D_OK, 2);
}
static void get_function(uint32_t kind) {   /* (this, void* data, UINT* size): NULL data asks the size */
    struct d3d_shader *s = d3d_host(A32(0), kind);
    uint32_t bytes = 4 * s->ntokens;
    if (!A32(2)) RET(D3DERR_INVALIDCALL, 3);
    if (!A32(1)) { MEM32(A32(2)) = bytes; RET(D3D_OK, 3); }
    if (MEM32(A32(2)) < bytes) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(1), s->tokens, bytes);
    MEM32(A32(2)) = bytes;
    RET(D3D_OK, 3);
}
static void vs_GetDevice(void) { get_device(D3DK_VSHADER); }
static void vs_GetFunction(void) { get_function(D3DK_VSHADER); }
static void ps_GetDevice(void) { get_device(D3DK_PSHADER); }
static void ps_GetFunction(void) { get_function(D3DK_PSHADER); }

static const win32hle_shim g_impls[] = {
    { "IDirect3DVertexShader9::GetDevice", vs_GetDevice },
    { "IDirect3DVertexShader9::GetFunction", vs_GetFunction },
    { "IDirect3DPixelShader9::GetDevice", ps_GetDevice },
    { "IDirect3DPixelShader9::GetFunction", ps_GetFunction },
    { 0, 0 } };
void d3d9_shader_register(void) { d3d_impls(g_impls); }
