/*
 * fx_states.c - the fx_2_0 state operations and passes (plan 5 Task 6).
 *
 * The operation numbers are D3DX's: render states first (0-102, D3DRS order
 * with WRAP0-15 together), texture stage states (103-120), NPatchMode, FVF,
 * the transforms (123-126), material (127-131) and light (132-144) members,
 * LightEnable, VertexShader (146), PixelShader (147), shader constants
 * (148-163), Texture (164), the sampler states (165-177) and Sampler (178).
 * The game's passes (Lighting 48, ClipPlaneEnable 59, ColorWriteEnable 73,
 * BlendOp 75 in their HLSL text; 146 and 147 once per pass) pin the numbering.
 *
 * A pass is applied through the device's own methods, called as the guest
 * calls them, so state blocks, Get calls and draws see what the game would.
 * Each shader a pass sets gets its constants from the parameters its
 * constant table names and from its preshader's outputs; CommitChanges
 * re-applies only what depends on a parameter set since (by dirty serial).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"
#include "sm.h"
#include "fx.h"

uint32_t g_fx_const_uploads;

#define NOPS 179u
#define CC_PRES 0x53455250u                  /* "PRES" */

/* ---- the table ---- */

static const uint16_t g_rs[103] = {
    7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 198, 199, 200, 201, 202, 203, 204, 205, 136, 137, 139, 140, 141,
    142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 165, 166, 167, 168,
    170, 171, 172, 173, 174, 175, 176, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192,
    193, 194, 195, 206, 207, 208, 209 };
static const uint8_t g_tss[18] = { 1, 26, 2, 3, 4, 27, 5, 6, 28, 7, 8, 9, 10, 11, 22, 23, 24, 32 };
static int rs_float(uint32_t rs) {
    return (rs >= 36 && rs <= 38) || rs == 154 || rs == 155 || (rs >= 158 && rs <= 160) || rs == 166 || rs == 170
        || rs == 175 || (rs >= 178 && rs <= 183) || rs == 195;
}
static int rs_color(uint32_t rs) { return rs == 34 || rs == 60 || rs == 139 || rs == 193; }

static struct fx_state_info g_info[NOPS];
static void init_table(void) {
    if (g_info[146].cls) return;
    for (uint32_t i = 0; i < 103; i++)
        g_info[i] = (struct fx_state_info){ FXS_RENDER, (uint8_t)(rs_float(g_rs[i]) ? FXV_FLOAT : rs_color(g_rs[i]) ? FXV_COLOR : FXV_DWORD),
                                            g_rs[i], "render state" };
    for (uint32_t i = 0; i < 18; i++) {
        uint32_t t = g_tss[i];
        g_info[103 + i] = (struct fx_state_info){ FXS_STAGE, (uint8_t)((t >= 7 && t <= 10) || t == 22 || t == 23 ? FXV_FLOAT
                                                  : t == 32 ? FXV_COLOR : FXV_DWORD), (uint16_t)t, "texture stage state" };
    }
    g_info[121] = (struct fx_state_info){ FXS_NPATCH, FXV_FLOAT, 0, "NPatchMode" };
    g_info[122] = (struct fx_state_info){ FXS_FVF, FXV_DWORD, 0, "FVF" };
    static const uint16_t xf[4] = { 3, 2, 256, 16 };                  /* PROJECTION, VIEW, WORLD, TEXTURE0 */
    for (uint32_t i = 0; i < 4; i++) g_info[123 + i] = (struct fx_state_info){ FXS_TRANSFORM, FXV_MATRIX, xf[i], "transform" };
    for (uint32_t i = 0; i < 4; i++) g_info[127 + i] = (struct fx_state_info){ FXS_MATERIAL, FXV_VEC4, (uint16_t)(16 * i), "material" };
    g_info[131] = (struct fx_state_info){ FXS_MATERIAL, FXV_FLOAT, 64, "material" };
    static const struct { uint8_t v; uint16_t off; } lt[13] = { { FXV_DWORD, 0 }, { FXV_VEC4, 4 }, { FXV_VEC4, 20 },
        { FXV_VEC4, 36 }, { FXV_VEC3, 52 }, { FXV_VEC3, 64 }, { FXV_FLOAT, 76 }, { FXV_FLOAT, 80 }, { FXV_FLOAT, 84 },
        { FXV_FLOAT, 88 }, { FXV_FLOAT, 92 }, { FXV_FLOAT, 96 }, { FXV_FLOAT, 100 } };
    for (uint32_t i = 0; i < 13; i++) g_info[132 + i] = (struct fx_state_info){ FXS_LIGHT, lt[i].v, lt[i].off, "light" };
    g_info[145] = (struct fx_state_info){ FXS_LIGHTENABLE, FXV_DWORD, 0, "LightEnable" };
    g_info[146] = (struct fx_state_info){ FXS_VSHADER, FXV_OBJECT, 0, "VertexShader" };
    g_info[147] = (struct fx_state_info){ FXS_PSHADER, FXV_OBJECT, 0, "PixelShader" };
    /* constants: num is 0x10 for the pixel shader's, plus 0 float, 1 bool, 2 int */
    static const uint8_t ck[8] = { 0, 1, 2, 0, 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t k = ck[i & 7];
        g_info[148 + i] = (struct fx_state_info){ FXS_CONST, (uint8_t)(k == 0 ? FXV_FLOATS : k == 1 ? FXV_BOOLS : FXV_INTS),
                                                  (uint16_t)((i >= 8 ? 0x10u : 0u) | k), "shader constant" };
    }
    g_info[164] = (struct fx_state_info){ FXS_TEXTURE, FXV_OBJECT, 0, "Texture" };
    for (uint32_t i = 1; i <= 13; i++)
        g_info[164 + i] = (struct fx_state_info){ FXS_SAMPLER, (uint8_t)(i == 4 ? FXV_COLOR : i == 8 ? FXV_FLOAT : FXV_DWORD),
                                                  (uint16_t)i, "sampler state" };
    g_info[178] = (struct fx_state_info){ FXS_SETSAMPLER, FXV_OBJECT, 0, "Sampler" };
}
const struct fx_state_info *fx_state(uint32_t op) {
    init_table();
    return op < NOPS ? &g_info[op] : NULL;
}
static const struct fx_state_info *info_of(const struct fx_state *s) {
    const struct fx_state_info *i = fx_state(s->op);
    if (!i) d3d_fail("ID3DXEffect: state operation %u has no table entry", s->op);
    return i;
}

/* ---- the device's methods, as the guest calls them ---- */

enum { D_SetTransform, D_SetMaterial, D_GetMaterial, D_SetLight, D_GetLight, D_LightEnable, D_SetRenderState,
       D_SetTexture, D_SetTextureStageState, D_SetSamplerState, D_SetNPatchMode, D_SetFVF, D_CreateVertexShader,
       D_SetVertexShader, D_SetVertexShaderConstantF, D_SetVertexShaderConstantI, D_SetVertexShaderConstantB,
       D_CreatePixelShader, D_SetPixelShader, D_SetPixelShaderConstantF, D_SetPixelShaderConstantI,
       D_SetPixelShaderConstantB, D_COUNT };
static const char *const g_dname[D_COUNT] = { "SetTransform", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "LightEnable", "SetRenderState", "SetTexture", "SetTextureStageState", "SetSamplerState", "SetNPatchMode",
    "SetFVF", "CreateVertexShader", "SetVertexShader", "SetVertexShaderConstantF", "SetVertexShaderConstantI",
    "SetVertexShaderConstantB", "CreatePixelShader", "SetPixelShader", "SetPixelShaderConstantF",
    "SetPixelShaderConstantI", "SetPixelShaderConstantB" };
static int g_dslot[D_COUNT];
static uint32_t dcall(uint32_t dev, int m, int n, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!g_dslot[D_SetTransform]) {
        const struct d3d_iface *f = d3d_ifaces;
        while (f->name && strcmp(f->name, "IDirect3DDevice9")) f++;
        if (!f->name) d3d_fail("ID3DXEffect: no IDirect3DDevice9 interface");
        for (int k = 0; k < D_COUNT; k++) {
            int i = 0;
            while (f->methods[i] && strcmp(f->methods[i], g_dname[k])) i++;
            if (!f->methods[i]) d3d_fail("ID3DXEffect: IDirect3DDevice9 has no %s", g_dname[k]);
            g_dslot[k] = i;
        }
    }
    uint32_t a[4] = { dev, a1, a2, a3 };
    return hle_call_guest(MEM32(MEM32(dev) + 4u * (uint32_t)g_dslot[m]), n + 1, a);
}

/* ---- values ---- */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int32_t f2i(float f) { return f > -2147483649.0f && f < 2147483648.0f ? (int32_t)f : INT32_MIN; }
static int numeric(const struct fx_param *p) {
    return p && p->data && p->cls <= FXC_MATRIX_COLUMNS && (p->type == FXT_BOOL || p->type == FXT_INT || p->type == FXT_FLOAT);
}
/* element (r, c) of a numeric parameter (its own layout) as a float; 0 outside it */
static float pnum(const struct fx_param *p, uint32_t r, uint32_t c) {
    if (!numeric(p) || r >= p->rows || c >= p->cols) return 0.0f;
    uint32_t v = rd32(p->data + 4 * (p->cls == FXC_MATRIX_COLUMNS ? c * p->rows + r : r * p->cols + c));
    return p->type == FXT_FLOAT ? u2f(v) : p->type == FXT_INT ? (float)(int32_t)v : (v ? 1.0f : 0.0f);
}
/* the i-th number of a numeric parameter in storage order, as a float */
static float pidx(const struct fx_param *p, uint32_t i) {
    if (!numeric(p) || 4 * i >= p->bytes) return 0.0f;
    uint32_t v = rd32(p->data + 4 * i);
    return p->type == FXT_FLOAT ? u2f(v) : p->type == FXT_INT ? (float)(int32_t)v : (v ? 1.0f : 0.0f);
}
static float sat(float f) { return f < 0 ? 0 : f > 1 ? 1 : f; }
/* a parameter as a state's DWORD of value type vt */
static uint32_t pdword(const struct fx_param *p, uint32_t vt) {
    if (!numeric(p)) return 0;
    if (vt == FXV_COLOR && p->type == FXT_FLOAT && p->cls == FXC_VECTOR && p->cols >= 3) {
        float a = p->cols > 3 ? pnum(p, 0, 3) : 1.0f;
        return (uint32_t)(sat(a) * 255.0f + 0.5f) << 24 | (uint32_t)(sat(pnum(p, 0, 0)) * 255.0f + 0.5f) << 16
             | (uint32_t)(sat(pnum(p, 0, 1)) * 255.0f + 0.5f) << 8 | (uint32_t)(sat(pnum(p, 0, 2)) * 255.0f + 0.5f);
    }
    uint32_t v = rd32(p->data);
    if (vt == FXV_FLOAT) return p->type == FXT_FLOAT ? v : f2u(pnum(p, 0, 0));
    return p->type == FXT_FLOAT ? (uint32_t)f2i(u2f(v)) : p->type == FXT_BOOL ? (v != 0) : v;
}

/* a state's expression (or a selector's index expression), parsed at first use */
static struct fx_expr *state_expr(struct fx_effect *e, struct fx_state *s) {
    if (s->expr) return s->expr;
    const uint8_t *b = s->blob;
    uint32_t n = s->bloblen;
    if (s->usage == FX_USAGE_SELECTOR) {                /* the array's name (size, bytes), then the expression */
        uint32_t len = n >= 4 ? rd32(b) : 0;
        uint32_t skip = 4 + len;
        if (n < 4 || !len || len > n - 4 || (len & 3u) || !memchr(b + 4, 0, len))
            d3d_fail("ID3DXEffect: state %u: a malformed array selector", s->op);
        s->ref = fx_find_param(e, NULL, (const char *)b + 4);
        if (!s->ref || !s->ref->elements)
            d3d_fail("ID3DXEffect: state %u selects from '%s', which is no array parameter", s->op, (const char *)b + 4);
        b += skip, n -= skip;
    }
    struct fx_expr *x = calloc(1, sizeof *x);
    char err[256];
    if (fx_expr_parse((const uint32_t *)b, n / 4, e, x, err, sizeof err))
        d3d_fail("ID3DXEffect: state %u's expression: %s", s->op, err);
    for (uint32_t i = 0; i < x->in.n; i++)
        if (!x->in.e[i].param) d3d_fail("ID3DXEffect: state %u's expression reads '%s', no parameter", s->op, x->in.e[i].name);
    s->expr = x;
    return x;
}
static int is_expression(const struct fx_state *s) {
    return s->usage == FX_USAGE_BLOB && !(s->param.cls == FXC_OBJECT && FX_IS_SHADER(s->param.type));
}
/* the parameter holding s's value now: its own, the one it names, or the array element its selector picks
 * (an expression's result is stored into s->param, converted to its type) */
static const struct fx_param *state_value(struct fx_effect *e, struct fx_state *s) {
    if (s->usage == FX_USAGE_PARAM) return s->ref;
    if (s->usage == FX_USAGE_SELECTOR) {
        struct fx_expr *x = state_expr(e, s);
        float f;
        fx_expr_run(x, e, &f, 1);
        int32_t i = f2i(f);
        if (i < 0 || (uint32_t)i >= s->ref->elements)
            d3d_fail("ID3DXEffect: state %u selects element %d of '%s', which has %u", s->op, i, s->ref->name, s->ref->elements);
        return &s->ref->members[i];
    }
    if (is_expression(s)) {
        struct fx_expr *x = state_expr(e, s);
        struct fx_param *p = &s->param;
        uint32_t n = p->bytes / 4;
        float out[64];
        if (n > 64) n = 64;
        fx_expr_run(x, e, out, n);
        if (numeric(p))
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = p->type == FXT_FLOAT ? f2u(out[i]) : p->type == FXT_INT ? (uint32_t)f2i(out[i]) : out[i] != 0.0f;
                memcpy(p->data + 4 * i, &v, 4);
            }
        return p;
    }
    return &s->param;
}
/* the newest serial s's value depends on */
static uint64_t state_serial(struct fx_effect *e, struct fx_state *s) {
    uint64_t v = 0;
    if (s->usage == FX_USAGE_PARAM && s->ref) v = *s->ref->serial;
    if (s->usage == FX_USAGE_SELECTOR || is_expression(s)) {
        struct fx_expr *x = state_expr(e, s);
        v = fx_expr_serial(x);
        if (s->ref && *s->ref->serial > v) v = *s->ref->serial;
    }
    return v;
}
static uint64_t sampler_serial(struct fx_effect *e, const struct fx_param *sp) {
    uint64_t v = sp->serial ? *sp->serial : 0;
    for (uint32_t i = 0; i < sp->nstates; i++) {
        uint64_t k = state_serial(e, &sp->states[i]);
        if (k > v) v = k;
    }
    return v;
}

/* ---- shaders ---- */

/* fxc writes "VertexShader = NULL;" (and Texture) as the number 0 */
static int null_object(const struct fx_param *p) {
    if (!numeric(p)) return 0;
    if (pdword(p, FXV_DWORD)) d3d_fail("ID3DXEffect: an object state set to the number %u", pdword(p, FXV_DWORD));
    return 1;
}
static struct fx_object *shader_object(struct fx_effect *e, const struct fx_param *p, uint32_t kind) {
    if (!p || p->cls != FXC_OBJECT || p->type != kind || p->elements || p->object >= e->nobjects)
        d3d_fail("ID3DXEffect: a %s state's value is not a %s", kind == FXT_VERTEXSHADER ? "VertexShader" : "PixelShader",
                 kind == FXT_VERTEXSHADER ? "vertex shader" : "pixel shader");
    return &e->objects[p->object];
}
static struct fx_shinfo *shinfo(struct fx_effect *e, struct fx_object *o) {
    if (o->sh) return o->sh;
    struct fx_shinfo *sh = calloc(1, sizeof *sh);
    const uint32_t *t = (const uint32_t *)o->data;
    uint32_t n = o->size / 4, len;
    char err[256];
    sh->version = t[0];
    if (fx_ctab_read(t, n, e, &sh->ctab, err, sizeof err)) d3d_fail("ID3DXEffect: shader 0x%08X: %s", t[0], err);
    for (uint32_t i = 0; i < sh->ctab.n; i++)
        if (!sh->ctab.e[i].param)
            d3d_fail("ID3DXEffect: shader 0x%08X's constant '%s' names no parameter", t[0], sh->ctab.e[i].name);
    const uint32_t *pre = fx_comment(t, n, CC_PRES, &len);
    if (pre && len) {                                   /* fxc also writes empty PRES comments */
        sh->pre = calloc(1, sizeof *sh->pre);
        if (fx_expr_parse(pre, len, e, sh->pre, err, sizeof err)) d3d_fail("ID3DXEffect: shader 0x%08X's preshader: %s", t[0], err);
        for (uint32_t i = 0; i < sh->pre->in.n; i++)
            if (!sh->pre->in.e[i].param)
                d3d_fail("ID3DXEffect: shader 0x%08X's preshader reads '%s', no parameter", t[0], sh->pre->in.e[i].name);
    }
    o->sh = sh;
    return sh;
}
void fx_shader_free(struct fx_object *o) {
    if (!o->sh) return;
    fx_ctab_free(&o->sh->ctab);
    if (o->sh->pre) fx_expr_free(o->sh->pre), free(o->sh->pre);
    free(o->sh);
    o->sh = NULL;
}
static uint32_t shader_guest(struct fx_effect *e, struct fx_object *o, uint32_t kind) {
    if (!o->size) return 0;
    if (!o->guest) {
        uint32_t mark = hle_scratch_mark(), out = hle_scratch(4);
        uint32_t hr = dcall(e->device, kind == FXT_VERTEXSHADER ? D_CreateVertexShader : D_CreatePixelShader, 2,
                            hle_va(o->data), out, 0);
        if (hr) d3d_fail("ID3DXEffect: the device refused the effect's shader 0x%08X (0x%08X)", MEM32(hle_va(o->data)), hr);
        o->guest = MEM32(out);
        hle_scratch_release(mark);
    }
    return o->guest;
}
/* each shader a shader state can set: fn(object) */
static void shader_candidates(struct fx_effect *e, struct fx_state *s, uint32_t kind,
                              void (*fn)(struct fx_effect *, struct fx_object *, void *), void *ctx) {
    if (s->usage == FX_USAGE_SELECTOR) {
        state_expr(e, s);
        for (uint32_t i = 0; i < s->ref->elements; i++) fn(e, shader_object(e, &s->ref->members[i], kind), ctx);
    } else if (s->usage == FX_USAGE_PARAM) fn(e, shader_object(e, s->ref, kind), ctx);
    else if (!null_object(&s->param)) fn(e, shader_object(e, &s->param, kind), ctx);
}

/* ---- applying ---- */

static void set_dword(struct fx_effect *e, const struct fx_state_info *in, uint32_t index, const struct fx_param *p) {
    uint32_t v = pdword(p, in->vtype);
    switch (in->cls) {
    case FXS_RENDER: dcall(e->device, D_SetRenderState, 2, in->num, v, 0); break;
    case FXS_STAGE: dcall(e->device, D_SetTextureStageState, 3, index, in->num, v); break;
    case FXS_SAMPLER: dcall(e->device, D_SetSamplerState, 3, index, in->num, v); break;
    case FXS_NPATCH: dcall(e->device, D_SetNPatchMode, 1, v, 0, 0); break;
    case FXS_FVF: dcall(e->device, D_SetFVF, 1, v, 0, 0); break;
    case FXS_LIGHTENABLE: dcall(e->device, D_LightEnable, 2, index, v, 0); break;
    }
}
static uint32_t texture_of(const struct fx_param *p) {
    if (null_object(p)) return 0;
    if (!p || p->cls != FXC_OBJECT || !FX_IS_TEXTURE(p->type) || p->elements || !p->data)
        d3d_fail("ID3DXEffect: a Texture state's value is not a texture");
    return rd32(p->data);
}
static void apply_sampler(struct fx_effect *e, struct fx_param *sp, uint32_t stage) {
    if (!sp || sp->cls != FXC_OBJECT || !FX_IS_SAMPLER(sp->type)) d3d_fail("ID3DXEffect: a Sampler state's value is not a sampler");
    for (uint32_t i = 0; i < sp->nstates; i++) {
        struct fx_state *s = &sp->states[i];
        const struct fx_state_info *in = info_of(s);
        const struct fx_param *v = state_value(e, s);
        if (in->cls == FXS_TEXTURE) dcall(e->device, D_SetTexture, 2, stage, texture_of(v), 0);
        else if (in->cls == FXS_SAMPLER) set_dword(e, in, stage, v);
        else d3d_fail("ID3DXEffect: sampler state operation %u (%s) in a sampler", s->op, in->name);
    }
}
static void upload(struct fx_effect *e, int ps, uint32_t set, uint32_t index, const void *regs, uint32_t n) {
    if (!n) return;
    uint32_t mark = hle_scratch_mark(), g = hle_scratch_copy(regs, set == FX_RS_BOOL ? 4 * n : 16 * n);
    if (set == FX_RS_BOOL) {                            /* one BOOL per register: the first dword of each */
        const uint32_t (*r)[4] = regs;
        for (uint32_t i = 0; i < n; i++) MEM32(g + 4 * i) = r[i][0];
    }
    int m = set == FX_RS_FLOAT4 ? (ps ? D_SetPixelShaderConstantF : D_SetVertexShaderConstantF)
          : set == FX_RS_INT4 ? (ps ? D_SetPixelShaderConstantI : D_SetVertexShaderConstantI)
          : (ps ? D_SetPixelShaderConstantB : D_SetVertexShaderConstantB);
    dcall(e->device, m, 3, index, g, n);
    g_fx_const_uploads++;
    hle_scratch_release(mark);
}
/* a shader's constants: the parameters its table names (all, or those set since the last apply) and its
 * preshader's outputs (when one of its inputs was set) */
static void shader_constants(struct fx_effect *e, struct fx_object *o, int ps, int all) {
    struct fx_shinfo *sh = shinfo(e, o);
    for (uint32_t i = 0; i < sh->ctab.n; i++) {
        struct fx_ctab_entry *x = &sh->ctab.e[i];
        if (x->set == FX_RS_SAMPLER) {
            for (uint32_t k = 0; k < x->count; k++) {
                struct fx_param *sp = x->param->elements ? (k < x->param->elements ? &x->param->members[k] : NULL) : (k ? NULL : x->param);
                if (!sp || (!all && sampler_serial(e, sp) <= e->applied)) continue;
                apply_sampler(e, sp, (ps ? 0u : 257u) + x->index + k);
            }
            continue;
        }
        if (!all && *x->param->serial <= e->applied) continue;
        uint32_t (*regs)[4] = calloc(x->count ? x->count : 1, 16);
        upload(e, ps, x->set, x->index, regs, fx_ctab_regs(&sh->ctab, i, regs, x->count));
        free(regs);
    }
    if (sh->pre && (all || fx_expr_serial(sh->pre) > e->applied)) {
        float *out = calloc(sh->pre->nout + 4, sizeof *out);
        fx_expr_run(sh->pre, e, out, sh->pre->nout);
        for (uint32_t r = 0; r < sh->pre->nranges; r++)
            upload(e, ps, FX_RS_FLOAT4, sh->pre->prsi[2 * r], out + 4 * sh->pre->prsi[2 * r], sh->pre->prsi[2 * r + 1]);
        free(out);
    }
}

static void apply_state(struct fx_effect *e, struct fx_state *s) {
    const struct fx_state_info *in = info_of(s);
    const struct fx_param *p = state_value(e, s);
    uint32_t mark = hle_scratch_mark();
    switch (in->cls) {
    case FXS_RENDER: case FXS_STAGE: case FXS_SAMPLER: case FXS_NPATCH: case FXS_FVF: case FXS_LIGHTENABLE:
        set_dword(e, in, s->index, p);
        break;
    case FXS_TRANSFORM: {
        uint32_t m = hle_scratch(64);
        for (uint32_t r = 0; r < 4; r++)
            for (uint32_t c = 0; c < 4; c++) MEM32(m + 16 * r + 4 * c) = f2u(pnum(p, r, c));
        dcall(e->device, D_SetTransform, 2, in->num + (in->num >= 16 ? s->index : 0), m, 0);
        break;
    }
    case FXS_MATERIAL: case FXS_LIGHT: {
        uint32_t b = hle_scratch(104);
        if (in->cls == FXS_MATERIAL) dcall(e->device, D_GetMaterial, 1, b, 0, 0);
        else if (dcall(e->device, D_GetLight, 2, s->index, b, 0)) {     /* unset: LightEnable's default light */
            MEM32(b) = 3, MEM32(b + 4) = MEM32(b + 8) = MEM32(b + 12) = f2u(1.0f), MEM32(b + 72) = f2u(1.0f);
        }
        uint32_t n = in->vtype == FXV_VEC4 ? 4 : in->vtype == FXV_VEC3 ? 3 : 1;
        for (uint32_t i = 0; i < n; i++)
            MEM32(b + in->num + 4 * i) = in->vtype == FXV_DWORD ? pdword(p, FXV_DWORD) : f2u(pidx(p, i));
        if (in->cls == FXS_MATERIAL) dcall(e->device, D_SetMaterial, 1, b, 0, 0);
        else dcall(e->device, D_SetLight, 2, s->index, b, 0);
        break;
    }
    case FXS_VSHADER: case FXS_PSHADER: {
        uint32_t kind = in->cls == FXS_VSHADER ? FXT_VERTEXSHADER : FXT_PIXELSHADER;
        if (null_object(p)) {                           /* "= NULL": the fixed-function stage */
            dcall(e->device, kind == FXT_VERTEXSHADER ? D_SetVertexShader : D_SetPixelShader, 1, 0, 0, 0);
            if (kind == FXT_VERTEXSHADER) e->vs_obj = NULL;
            else e->ps_obj = NULL;
            break;
        }
        struct fx_object *o = shader_object(e, p, kind);
        dcall(e->device, kind == FXT_VERTEXSHADER ? D_SetVertexShader : D_SetPixelShader, 1, shader_guest(e, o, kind), 0, 0);
        if (kind == FXT_VERTEXSHADER) e->vs_obj = o->size ? o : NULL;
        else e->ps_obj = o->size ? o : NULL;
        break;
    }
    case FXS_CONST: {
        uint32_t k = in->num & 0xF, n = numeric(p) ? p->bytes / 4 : 0, regs = k == 1 ? n : (n + 3) / 4;
        uint32_t g = hle_scratch(16 * (regs ? regs : 1));
        for (uint32_t i = 0; i < n; i++) {
            float f = pidx(p, i);
            MEM32(g + 4 * i) = k == 0 ? f2u(f) : k == 1 ? (f != 0.0f) : (uint32_t)f2i(f);
        }
        int ps = in->num & 0x10;
        int m = k == 0 ? (ps ? D_SetPixelShaderConstantF : D_SetVertexShaderConstantF)
              : k == 1 ? (ps ? D_SetPixelShaderConstantB : D_SetVertexShaderConstantB)
              : (ps ? D_SetPixelShaderConstantI : D_SetVertexShaderConstantI);
        if (regs) dcall(e->device, m, 3, s->index, g, regs), g_fx_const_uploads++;
        break;
    }
    case FXS_TEXTURE: dcall(e->device, D_SetTexture, 2, s->index, texture_of(p), 0); break;
    case FXS_SETSAMPLER: apply_sampler(e, (struct fx_param *)p, s->index); break;
    }
    hle_scratch_release(mark);
}

void fx_apply_pass(struct fx_effect *e, struct fx_pass *ps, int all) {
    struct fx_object *vs0 = e->vs_obj, *ps0 = e->ps_obj;
    if (all) e->vs_obj = e->ps_obj = NULL;
    for (uint32_t i = 0; i < ps->nstates; i++) {
        struct fx_state *s = &ps->states[i];
        int changed = all || state_serial(e, s) > e->applied;
        if (!changed && s->op == 178 && s->usage != FX_USAGE_SELECTOR)       /* Sampler[i]: its states' values too */
            changed = sampler_serial(e, s->usage == FX_USAGE_PARAM ? s->ref : &s->param) > e->applied;
        if (changed) apply_state(e, s);
    }
    if (e->vs_obj) shader_constants(e, e->vs_obj, 0, all || e->vs_obj != vs0);
    if (e->ps_obj) shader_constants(e, e->ps_obj, 1, all || e->ps_obj != ps0);
    e->applied = g_fx_serial;
}

/* ---- validation ---- */

static int in_range(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }
/* a constant value's legality, for the enumerated states */
static int legal(const struct fx_state_info *in, uint32_t v) {
    if (in->cls == FXS_RENDER)
        switch (in->num) {
        case 7: return v <= 2;                                          /* ZENABLE */
        case 8: case 9: case 22: return in_range(v, 1, 3);              /* FILLMODE, SHADEMODE, CULLMODE */
        case 23: case 25: case 56: case 189: return in_range(v, 1, 8);  /* the comparison functions */
        case 19: case 20: case 207: case 208: return in_range(v, 1, 17);   /* blend factors */
        case 171: case 209: return in_range(v, 1, 5);                   /* BLENDOP, BLENDOPALPHA */
        case 53: case 54: case 55: case 186: case 187: case 188: return in_range(v, 1, 8);   /* stencil ops */
        case 35: case 140: return v <= 3;                               /* fog modes */
        }
    if (in->cls == FXS_STAGE && (in->num == 1 || in->num == 4)) return in_range(v, 1, 26);   /* COLOROP, ALPHAOP */
    if (in->cls == FXS_SAMPLER) {
        if (in->num <= 3) return in_range(v, 1, 5);                     /* ADDRESSU/V/W */
        if (in->num >= 5 && in->num <= 7) return v <= 7;                /* MAG/MIN/MIPFILTER */
    }
    return 1;
}
static int g_shaders_ok;
static void check_shader(struct fx_effect *e, struct fx_object *o, void *ctx) {
    (void)e;
    uint32_t kind = *(uint32_t *)ctx;
    if (!o->size) return;
    struct sm_shader s;
    char err[256];
    if (o->size < 8 || sm_decode((const uint32_t *)o->data, o->size / 4, &s, err, sizeof err)) { g_shaders_ok = 0; return; }
    if (s.is_ps != (kind == FXT_PIXELSHADER) || s.major > 2 || (s.major == 2 && s.minor > 0)) g_shaders_ok = 0;
    sm_free(&s);
}
int fx_tech_valid(struct fx_effect *e, struct fx_technique *t) {
    for (uint32_t p = 0; p < t->npasses; p++)
        for (uint32_t i = 0; i < t->passes[p].nstates; i++) {
            struct fx_state *s = &t->passes[p].states[i];
            const struct fx_state_info *in = info_of(s);
            switch (in->cls) {
            case FXS_STAGE: if (s->index >= 8) return 0; break;
            case FXS_SAMPLER: case FXS_TEXTURE: case FXS_SETSAMPLER:
                if (s->index >= 16 && !in_range(s->index, 257, 260)) return 0;
                break;
            case FXS_TRANSFORM: if (s->index >= (in->num == 16 ? 8u : in->num == 256 ? 256u : 1u)) return 0; break;
            case FXS_VSHADER: case FXS_PSHADER: {
                uint32_t kind = in->cls == FXS_VSHADER ? FXT_VERTEXSHADER : FXT_PIXELSHADER;
                g_shaders_ok = 1;
                shader_candidates(e, s, kind, check_shader, &kind);
                if (!g_shaders_ok) return 0;
                break;
            }
            }
            if (s->usage == FX_USAGE_NONE && in->vtype == FXV_DWORD && numeric(&s->param) && !legal(in, pdword(&s->param, FXV_DWORD)))
                return 0;
        }
    return 1;
}

/* ---- what a technique uses ---- */

struct used { const struct fx_param *top; int hit; };
static void use(struct used *u, const struct fx_param *p) { if (p && p->top == u->top) u->hit = 1; }
static void use_expr(struct used *u, const struct fx_expr *x) { for (uint32_t i = 0; i < x->in.n; i++) use(u, x->in.e[i].param); }
static void use_state(struct fx_effect *e, struct used *u, struct fx_state *s);
static void use_sampler(struct fx_effect *e, struct used *u, const struct fx_param *sp) {
    use(u, sp);
    for (uint32_t i = 0; sp && i < sp->nstates; i++) use_state(e, u, &sp->states[i]);
}
static void use_shader(struct fx_effect *e, struct fx_object *o, void *ctx) {
    struct used *u = ctx;
    if (!o->size) return;
    struct fx_shinfo *sh = shinfo(e, o);
    for (uint32_t i = 0; i < sh->ctab.n; i++) {
        struct fx_param *p = sh->ctab.e[i].param;
        if (sh->ctab.e[i].set != FX_RS_SAMPLER) { use(u, p); continue; }
        if (!p->elements) use_sampler(e, u, p);
        else for (uint32_t k = 0; k < p->elements; k++) use_sampler(e, u, &p->members[k]);
    }
    if (sh->pre) use_expr(u, sh->pre);
}
static void use_state(struct fx_effect *e, struct used *u, struct fx_state *s) {
    const struct fx_state_info *in = info_of(s);
    if (s->usage == FX_USAGE_PARAM) use(u, s->ref);
    if (s->usage == FX_USAGE_SELECTOR || is_expression(s)) use_expr(u, state_expr(e, s)), use(u, s->ref);
    if (in->cls == FXS_VSHADER || in->cls == FXS_PSHADER)
        shader_candidates(e, s, in->cls == FXS_VSHADER ? FXT_VERTEXSHADER : FXT_PIXELSHADER, use_shader, u);
    if (in->cls == FXS_SETSAMPLER && s->usage == FX_USAGE_PARAM) use_sampler(e, u, s->ref);
}
int fx_param_used(struct fx_effect *e, struct fx_param *p, struct fx_technique *t) {
    struct used u = { p->top, 0 };
    for (uint32_t k = 0; k < t->npasses && !u.hit; k++)
        for (uint32_t i = 0; i < t->passes[k].nstates && !u.hit; i++) use_state(e, &u, &t->passes[k].states[i]);
    return u.hit;
}

/* ---- Begin: a state block of everything the technique's passes touch ---- */

static uint32_t smp_slot(uint32_t i) { return i >= 257 ? 16 + (i - 257) : i; }
static void mark_light(struct d3d_sbmask *m, uint32_t i) {
    for (uint32_t k = 0; k < m->nlight; k++) if (m->light[k] == i) return;
    m->light = realloc(m->light, sizeof *m->light * (m->nlight + 1));
    m->light[m->nlight++] = i;
}
static void mark_range(uint8_t *a, uint32_t size, uint32_t first, uint32_t n) {
    for (uint32_t i = first; i < first + n && i < size; i++) a[i] = 1;
}
static void mark_sampler(struct d3d_sbmask *m, uint32_t stage) {
    uint32_t i = smp_slot(stage);
    if (i >= NSMP) return;
    m->tex[i] = 1;
    for (uint32_t k = 1; k < NSS; k++) m->ss[i][k] = 1;
}
struct mark { struct d3d_sbmask *m; int ps; uint32_t flags; };
static void mark_shader(struct fx_effect *e, struct fx_object *o, void *ctx) {
    struct mark *k = ctx;
    if (!o->size) return;
    struct fx_shinfo *sh = shinfo(e, o);
    for (uint32_t i = 0; i < sh->ctab.n; i++) {
        struct fx_ctab_entry *x = &sh->ctab.e[i];
        if (x->set == FX_RS_SAMPLER) {
            if (!(k->flags & 4)) for (uint32_t j = 0; j < x->count; j++) mark_sampler(k->m, (k->ps ? 0u : 257u) + x->index + j);
        } else if (!(k->flags & 2)) {
            uint8_t *a = x->set == FX_RS_FLOAT4 ? (k->ps ? k->m->psf : k->m->vsf) : x->set == FX_RS_INT4 ? (k->ps ? k->m->psi : k->m->vsi)
                       : (k->ps ? k->m->psb : k->m->vsb);
            uint32_t size = x->set == FX_RS_FLOAT4 ? (k->ps ? 224u : 256u) : 16u;
            mark_range(a, size, x->index, x->count);
        }
    }
    if (sh->pre && !(k->flags & 2))
        for (uint32_t r = 0; r < sh->pre->nranges; r++)
            mark_range(k->ps ? k->m->psf : k->m->vsf, k->ps ? 224u : 256u, sh->pre->prsi[2 * r], sh->pre->prsi[2 * r + 1]);
}
uint32_t fx_begin(struct fx_effect *e, uint32_t flags) {
    struct d3d_sbmask m;
    memset(&m, 0, sizeof m);
    struct fx_technique *t = e->cur;
    for (uint32_t p = 0; p < t->npasses; p++)
        for (uint32_t i = 0; i < t->passes[p].nstates; i++) {
            struct fx_state *s = &t->passes[p].states[i];
            const struct fx_state_info *in = info_of(s);
            switch (in->cls) {
            case FXS_RENDER: m.rs[in->num] = 1; break;
            case FXS_STAGE: if (s->index < NTS) m.tss[s->index][in->num] = 1; break;
            case FXS_NPATCH: m.npatch = 1; break;
            case FXS_FVF: m.vdecl = 1; break;
            case FXS_TRANSFORM: m.xform[(in->num + (in->num >= 16 ? s->index : 0)) & 511] = 1; break;
            case FXS_MATERIAL: m.material = 1; break;
            case FXS_LIGHT: case FXS_LIGHTENABLE: mark_light(&m, s->index); break;
            case FXS_VSHADER: case FXS_PSHADER: {
                int ps = in->cls == FXS_PSHADER;
                if (!(flags & 2)) { if (ps) m.ps = 1; else m.vs = 1; }
                struct mark k = { &m, ps, flags };
                shader_candidates(e, s, ps ? FXT_PIXELSHADER : FXT_VERTEXSHADER, mark_shader, &k);
                break;
            }
            case FXS_CONST:
                if (!(flags & 2)) {
                    int ps = in->num & 0x10;
                    uint32_t kd = in->num & 0xF, n = s->param.bytes / 4, regs = kd == 1 ? n : (n + 3) / 4;
                    uint8_t *a = kd == 0 ? (ps ? m.psf : m.vsf) : kd == 1 ? (ps ? m.psb : m.vsb) : (ps ? m.psi : m.vsi);
                    mark_range(a, kd == 0 ? (ps ? 224u : 256u) : 16u, s->index, regs);
                }
                break;
            case FXS_TEXTURE: if (smp_slot(s->index) < NSMP) m.tex[smp_slot(s->index)] = 1; break;
            case FXS_SAMPLER: if (!(flags & 4) && smp_slot(s->index) < NSMP) m.ss[smp_slot(s->index)][in->num] = 1; break;
            case FXS_SETSAMPLER: if (!(flags & 4)) mark_sampler(&m, s->index); break;
            }
        }
    uint32_t sb = d3d_stateblock_new(d3d_dev(e->device), &m);
    free(m.light);
    return sb;
}
