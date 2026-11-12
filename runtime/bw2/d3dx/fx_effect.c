/*
 * fx_effect.c - ID3DXEffect's ID3DXBaseEffect methods (descriptions, handles
 * and parameter values), D3DXCreateEffect and D3DXCreateEffectPool (plan 5
 * Task 5), and CloneEffect, which the game calls on every effect it loads.
 * Techniques and passes are Task 6's (fx_states.c, fx_expr.c).
 *
 * Values follow D3DX's conversions: numbers convert between bool, int and
 * float on the way in and out (a float set on an int truncates, a bool reads
 * 0/1); an int set on a float3/float4 vector is a D3DCOLOR and the reverse;
 * matrices are stored as declared (rows or columns) and converted at
 * Set/Get; textures (and shaders) in values hold a reference. A call on the
 * wrong kind of parameter gives D3DERR_INVALIDCALL.
 *
 * A pool holds one value per shared parameter name: effects created with the
 * pool bind their shared parameters to it, and a type that differs from the
 * pool's fails D3DXCreateEffect with D3DERR_INVALIDCALL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "d3d_com.h"
#include "d3dx.h"
#include "fx.h"

#define FX_MAGIC 0xFEFF0901u

struct fx_com {
    struct d3d_obj base;
    struct fx_effect fx;
    uint32_t device, pool;                         /* both referenced; pool may be 0 */
    uint32_t flags;                                /* D3DXCreateEffect's */
};
#define D3DXFX_NOT_CLONEABLE 0x800u
struct fx_shared {
    char *name, *sig;
    uint8_t *data;
    uint32_t bytes, *objoff, nobj;                  /* the offsets of texture and shader slots in data */
    uint64_t serial;
    struct fx_shared *next;
};
struct fx_pool {
    struct d3d_obj base;
    struct fx_shared *list;
};

struct fx_effect *fx_effect_of(uint32_t effect) { return &((struct fx_com *)d3d_host(effect, D3DK_EFFECT))->fx; }
#define FX(i) (&((struct fx_com *)d3d_host(A32(i), D3DK_EFFECT))->fx)

/* ---- references held by values (a texture or shader is any guest COM object) ---- */

static void com_addref(uint32_t o) {
    if (!o) return;
    if (d3d_alive(o)) { d3d_addref(o); return; }
    uint32_t a[1] = { o };
    hle_call_guest(MEM32(MEM32(o) + 4), 1, a);
}
static void com_release(uint32_t o) {
    if (!o) return;
    if (d3d_alive(o)) { d3d_release(o); return; }
    uint32_t a[1] = { o };
    hle_call_guest(MEM32(MEM32(o) + 8), 1, a);
}
static int holds_ref(const struct fx_param *p) {
    return p->cls == FXC_OBJECT && (FX_IS_TEXTURE(p->type) || FX_IS_SHADER(p->type));
}
static void release_leaf(struct fx_param *leaf, void *ctx) {
    (void)ctx;
    if (holds_ref(leaf) && leaf->data) {
        uint32_t v;
        memcpy(&v, leaf->data, 4);
        com_release(v);
        memset(leaf->data, 0, 4);
    }
}
static void addref_leaf(struct fx_param *leaf, void *ctx) {
    (void)ctx;
    if (holds_ref(leaf) && leaf->data) { uint32_t v; memcpy(&v, leaf->data, 4); com_addref(v); }
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float gfl(uint32_t va) { return u2f(MEM32(va)); }
static void dirty(struct fx_param *p) { *p->serial = ++g_fx_serial; }

/* ---- numbers: a dword of type `from` as type `to` ---- */

static int32_t f2i(float f) {                       /* truncation, as cvttss2si (out of range: 0x80000000) */
    return f > -2147483649.0f && f < 2147483648.0f ? (int32_t)f : INT32_MIN;
}
static uint32_t conv(uint32_t v, uint32_t from, uint32_t to) {
    switch (to) {
    case FXT_FLOAT: return from == FXT_FLOAT ? v : from == FXT_INT ? f2u((float)(int32_t)v) : f2u(v ? 1.0f : 0.0f);
    case FXT_INT: return from == FXT_FLOAT ? (uint32_t)f2i(u2f(v)) : from == FXT_BOOL ? (v != 0) : v;
    case FXT_BOOL: return v != 0;                    /* any non-zero bits, -0.0f included */
    }
    return v;
}
static int numeric(const struct fx_param *p) {
    return p && p->cls <= FXC_MATRIX_COLUMNS && (p->type == FXT_BOOL || p->type == FXT_INT || p->type == FXT_FLOAT);
}
static int scalar1(const struct fx_param *p) { return numeric(p) && !p->elements && p->rows == 1 && p->cols == 1; }
static int matrix(const struct fx_param *p) {
    return numeric(p) && (p->cls == FXC_MATRIX_ROWS || p->cls == FXC_MATRIX_COLUMNS);
}
/* the colour form: an int on a float vector of 3 or 4 */
static int colour_vec(const struct fx_param *p) {
    return p && p->type == FXT_FLOAT && p->cls == FXC_VECTOR && !p->elements && p->cols >= 3;
}
static float sat(float f) { return f < 0 ? 0 : f > 1 ? 1 : f; }

/* a matrix's element r, c (4x4 user layout) in its storage */
static uint32_t midx(const struct fx_param *p, uint32_t r, uint32_t c) {
    return p->cls == FXC_MATRIX_ROWS ? r * p->cols + c : c * p->rows + r;
}
static void mat_set(struct fx_param *p, uint32_t m, int transpose) {
    for (uint32_t r = 0; r < p->rows; r++)
        for (uint32_t c = 0; c < p->cols; c++) {
            uint32_t v = MEM32(m + 4 * (transpose ? 4 * c + r : 4 * r + c));
            wr32(p->data + 4 * midx(p, r, c), conv(v, FXT_FLOAT, p->type));
        }
}
static void mat_get(const struct fx_param *p, uint32_t m, int transpose) {
    for (uint32_t r = 0; r < 4; r++)
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t v = r < p->rows && c < p->cols ? conv(rd32(p->data + 4 * midx(p, r, c)), p->type, FXT_FLOAT) : 0;
            MEM32(m + 4 * (transpose ? 4 * c + r : 4 * r + c)) = v;
        }
}

/* ---- descriptions and handles ---- */

static void x_GetDesc(void) {                       /* (this, D3DXEFFECT_DESC*) */
    struct fx_effect *e = FX(0);
    uint32_t d = A32(1);
    if (!d) RET(D3DERR_INVALIDCALL, 2);
    MEM32(d) = 0, MEM32(d + 4) = e->nparams, MEM32(d + 8) = e->ntechs, MEM32(d + 12) = 0;
    RET(D3D_OK, 2);
}
static void x_GetParameterDesc(void) {              /* (this, h, D3DXPARAMETER_DESC*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t d = A32(2);
    if (!p || !d) RET(D3DERR_INVALIDCALL, 3);
    uint32_t v[11] = { p->gname, p->gsemantic, p->cls, p->type, p->rows, p->cols, p->elements,
                       p->nannos, p->elements ? (p->members ? p->members[0].nmembers : 0) : p->nmembers,
                       p->flags, p->bytes };
    if (p->cls == FXC_OBJECT || p->cls == FXC_STRUCT) v[4] = v[5] = 0;
    for (int i = 0; i < 11; i++) MEM32(d + 4 * (uint32_t)i) = v[i];
    RET(D3D_OK, 3);
}
static void x_GetTechniqueDesc(void) {              /* (this, h, D3DXTECHNIQUE_DESC*) */
    struct fx_technique *t = fx_tech_of(FX(0), A32(1));
    uint32_t d = A32(2);
    if (!t || !d) RET(D3DERR_INVALIDCALL, 3);
    MEM32(d) = t->gname, MEM32(d + 4) = t->npasses, MEM32(d + 8) = t->nannos;
    RET(D3D_OK, 3);
}
/* a pass's shader of kind (FXT_VERTEXSHADER/FXT_PIXELSHADER): the guest address of its tokens, or 0 */
static uint32_t pass_shader(struct fx_effect *e, const struct fx_pass *ps, uint32_t kind) {
    for (uint32_t i = 0; i < ps->nstates; i++) {
        const struct fx_param *v = &ps->states[i].param;
        if (v->cls != FXC_OBJECT || v->type != kind || v->elements) continue;
        if (ps->states[i].usage == FX_USAGE_PARAM && ps->states[i].ref) v = ps->states[i].ref;
        if (v->cls != FXC_OBJECT || v->type != kind || v->elements) continue;
        const struct fx_object *o = &e->objects[v->object];
        return o->size ? hle_va(o->data) : 0;
    }
    return 0;
}
static void x_GetPassDesc(void) {                   /* (this, h, D3DXPASS_DESC*) */
    struct fx_effect *e = FX(0);
    struct fx_pass *ps = fx_pass_of(e, A32(1));
    uint32_t d = A32(2);
    if (!ps || !d) RET(D3DERR_INVALIDCALL, 3);
    MEM32(d) = ps->gname, MEM32(d + 4) = ps->nannos;
    MEM32(d + 8) = pass_shader(e, ps, FXT_VERTEXSHADER), MEM32(d + 12) = pass_shader(e, ps, FXT_PIXELSHADER);
    RET(D3D_OK, 3);
}
static void x_GetFunctionDesc(void) { (void)FX(0); RET(D3DERR_INVALIDCALL, 3); }   /* fx_2_0 has no functions */
static void x_GetFunction(void) { (void)FX(0); RET(0, 2); }
static void x_GetFunctionByName(void) { (void)FX(0); RET(0, 2); }

static void x_GetParameter(void) {                  /* (this, hParent, index) */
    struct fx_effect *e = FX(0);
    uint32_t i = A32(2);
    if (!A32(1)) RET(i < e->nparams ? e->params[i].handle : 0, 3);
    struct fx_param *p = fx_param_of(e, A32(1));
    RET(p && !p->elements && p->cls == FXC_STRUCT && i < p->nmembers ? p->members[i].handle : 0, 3);
}
static void x_GetParameterByName(void) {            /* (this, hParent, name) */
    struct fx_effect *e = FX(0);
    const char *name = ASTR(2);
    struct fx_param *parent = A32(1) ? fx_param_of(e, A32(1)) : NULL;
    if (!name || (A32(1) && !parent)) RET(0, 3);
    RET(fx_handle(fx_find_param(e, parent, name)), 3);
}
static void x_GetParameterBySemantic(void) {        /* (this, hParent, semantic) */
    struct fx_effect *e = FX(0);
    const char *sem = ASTR(2);
    struct fx_param *parent = A32(1) ? fx_param_of(e, A32(1)) : NULL, *list = e->params;
    uint32_t n = e->nparams;
    if (A32(1)) {
        if (!parent || parent->elements || parent->cls != FXC_STRUCT) RET(0, 3);
        list = parent->members, n = parent->nmembers;
    }
    if (sem)
        for (uint32_t i = 0; i < n; i++)
            if (list[i].semantic && !strcasecmp(list[i].semantic, sem)) RET(list[i].handle, 3);
    RET(0, 3);
}
static void x_GetParameterElement(void) {           /* (this, h, index) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    RET(p && A32(2) < p->elements ? p->members[A32(2)].handle : 0, 3);
}
static void x_GetTechnique(void) {                  /* (this, index) */
    struct fx_effect *e = FX(0);
    RET(A32(1) < e->ntechs ? e->techs[A32(1)].handle : 0, 2);
}
static void x_GetTechniqueByName(void) {            /* (this, name) */
    struct fx_effect *e = FX(0);
    const char *name = ASTR(1);
    for (uint32_t i = 0; name && i < e->ntechs; i++)
        if (e->techs[i].name && !strcmp(e->techs[i].name, name)) RET(e->techs[i].handle, 2);
    RET(0, 2);
}
static void x_GetPass(void) {                       /* (this, hTechnique, index) */
    struct fx_technique *t = fx_tech_of(FX(0), A32(1));
    RET(t && A32(2) < t->npasses ? t->passes[A32(2)].handle : 0, 3);
}
static void x_GetPassByName(void) {                 /* (this, hTechnique, name) */
    struct fx_technique *t = fx_tech_of(FX(0), A32(1));
    const char *name = ASTR(2);
    for (uint32_t i = 0; t && name && i < t->npasses; i++)
        if (t->passes[i].name && !strcmp(t->passes[i].name, name)) RET(t->passes[i].handle, 3);
    RET(0, 3);
}
/* the annotations of whatever a handle names (a parameter, technique or pass) */
static int annos_of(struct fx_effect *e, uint32_t h, struct fx_param **a, uint32_t *n) {
    struct fx_param *p = fx_param_of(e, h);
    struct fx_technique *t = p ? NULL : fx_tech_of(e, h);
    struct fx_pass *ps = p || t ? NULL : fx_pass_of(e, h);
    if (p) *a = p->annos, *n = p->nannos;
    else if (t) *a = t->annos, *n = t->nannos;
    else if (ps) *a = ps->annos, *n = ps->nannos;
    else return 0;
    return 1;
}
static void x_GetAnnotation(void) {                 /* (this, h, index) */
    struct fx_param *a;
    uint32_t n;
    if (!annos_of(FX(0), A32(1), &a, &n) || A32(2) >= n) RET(0, 3);
    RET(a[A32(2)].handle, 3);
}
static void x_GetAnnotationByName(void) {           /* (this, h, name) */
    struct fx_param *a;
    uint32_t n;
    const char *name = ASTR(2);
    if (!name || !annos_of(FX(0), A32(1), &a, &n)) RET(0, 3);
    for (uint32_t i = 0; i < n; i++)
        if (a[i].name && !strcmp(a[i].name, name)) RET(a[i].handle, 3);
    RET(0, 3);
}

/* ---- values ---- */

static void swap_refs(struct fx_param *leaf, void *ctx) {     /* ctx: the new value's bytes, at leaf's offset */
    const uint8_t *src = ctx;
    if (!holds_ref(leaf)) return;
    uint32_t nv = rd32(src + (leaf->data - leaf->top->data)), ov = rd32(leaf->data);
    if (nv != ov) com_addref(nv), com_release(ov);
}
static void x_SetValue(void) {                      /* (this, h, data, bytes) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    const uint8_t *src = APTR(2);
    if (!p || !src || !p->bytes || A32(3) < p->bytes) RET(D3DERR_INVALIDCALL, 4);
    /* swap_refs finds each leaf's offset from its top; point a copy of the value as the top's would be */
    uint8_t *img = malloc(p->top->bytes ? p->top->bytes : 1);
    memcpy(img, p->top->data, p->top->bytes);
    memcpy(img + (p->data - p->top->data), src, p->bytes);
    fx_leaves(p, swap_refs, img);
    free(img);
    memcpy(p->data, src, p->bytes);
    dirty(p);
    RET(D3D_OK, 4);
}
static void x_GetValue(void) {                      /* (this, h, data, bytes) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!p || !A32(2) || !p->bytes || A32(3) < p->bytes) RET(D3DERR_INVALIDCALL, 4);
    fx_leaves(p, addref_leaf, NULL);
    memcpy(APTR(2), p->data, p->bytes);
    RET(D3D_OK, 4);
}

static void x_SetBool(void) {                       /* (this, h, BOOL) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!scalar1(p)) RET(D3DERR_INVALIDCALL, 3);
    wr32(p->data, conv(A32(2), FXT_BOOL, p->type));
    dirty(p);
    RET(D3D_OK, 3);
}
static void x_GetBool(void) {                       /* (this, h, BOOL*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!scalar1(p) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = conv(rd32(p->data), p->type, FXT_BOOL);
    RET(D3D_OK, 3);
}
static void x_SetInt(void) {                        /* (this, h, INT) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t n = A32(2);
    if (scalar1(p)) wr32(p->data, conv(n, FXT_INT, p->type));
    else if (colour_vec(p)) {                       /* a D3DCOLOR: r, g, b (and a) */
        wr32(p->data, f2u((float)(n >> 16 & 0xFF) / 255.0f));
        wr32(p->data + 4, f2u((float)(n >> 8 & 0xFF) / 255.0f));
        wr32(p->data + 8, f2u((float)(n & 0xFF) / 255.0f));
        if (p->cols > 3) wr32(p->data + 12, f2u((float)(n >> 24) / 255.0f));
    } else RET(D3DERR_INVALIDCALL, 3);
    dirty(p);
    RET(D3D_OK, 3);
}
static void x_GetInt(void) {                        /* (this, h, INT*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t out = A32(2);
    if (!out) RET(D3DERR_INVALIDCALL, 3);
    if (scalar1(p)) MEM32(out) = conv(rd32(p->data), p->type, FXT_INT);
    else if (colour_vec(p)) {
        uint32_t n = (uint32_t)(sat(u2f(rd32(p->data + 8))) * 255.0f);
        n += (uint32_t)(sat(u2f(rd32(p->data + 4))) * 255.0f) << 8;
        n += (uint32_t)(sat(u2f(rd32(p->data))) * 255.0f) << 16;
        if (p->cols > 3) n += (uint32_t)(sat(u2f(rd32(p->data + 12))) * 255.0f) << 24;
        MEM32(out) = n;
    } else RET(D3DERR_INVALIDCALL, 3);
    RET(D3D_OK, 3);
}
static void x_SetFloat(void) {                      /* (this, h, FLOAT) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!scalar1(p)) RET(D3DERR_INVALIDCALL, 3);
    wr32(p->data, conv(A32(2), FXT_FLOAT, p->type));
    dirty(p);
    RET(D3D_OK, 3);
}
static void x_GetFloat(void) {                      /* (this, h, FLOAT*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!scalar1(p) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = conv(rd32(p->data), p->type, FXT_FLOAT);
    RET(D3D_OK, 3);
}
/* Set/Get{Bool,Int,Float}Array: the first min(count, size) numbers of any scalar, vector or matrix
 * parameter (or array of them), converted one by one. SetBoolArray's values are taken as ints. */
static void set_array(uint32_t from) {              /* (this, h, values*, count) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t src = A32(2), n = A32(3);
    if (!numeric(p) || (!src && n)) RET(D3DERR_INVALIDCALL, 4);
    if (n > p->bytes / 4) n = p->bytes / 4;
    for (uint32_t i = 0; i < n; i++) wr32(p->data + 4 * i, conv(MEM32(src + 4 * i), from, p->type));
    dirty(p);
    RET(D3D_OK, 4);
}
static void get_array(uint32_t to) {                /* (this, h, values*, count) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t dst = A32(2), n = A32(3);
    if (!numeric(p) || !dst) RET(D3DERR_INVALIDCALL, 4);
    if (n > p->bytes / 4) n = p->bytes / 4;
    for (uint32_t i = 0; i < n; i++) MEM32(dst + 4 * i) = conv(rd32(p->data + 4 * i), p->type, to);
    RET(D3D_OK, 4);
}
static void x_SetBoolArray(void) { set_array(FXT_INT); }
static void x_GetBoolArray(void) { get_array(FXT_BOOL); }
static void x_SetIntArray(void) { set_array(FXT_INT); }
static void x_GetIntArray(void) { get_array(FXT_INT); }
static void x_SetFloatArray(void) { set_array(FXT_FLOAT); }
static void x_GetFloatArray(void) { get_array(FXT_FLOAT); }

static int vec_like(const struct fx_param *p) { return numeric(p) && (p->cls == FXC_SCALAR || p->cls == FXC_VECTOR); }
static void vec_set(struct fx_param *p, uint32_t v) {
    if (p->type == FXT_INT && p->bytes == 4) {      /* an int vector of one: a D3DCOLOR */
        uint32_t n = (uint32_t)(sat(gfl(v + 8)) * 255.0f) + ((uint32_t)(sat(gfl(v + 4)) * 255.0f) << 8)
                   + ((uint32_t)(sat(gfl(v)) * 255.0f) << 16) + ((uint32_t)(sat(gfl(v + 12)) * 255.0f) << 24);
        wr32(p->data, n);
        return;
    }
    for (uint32_t i = 0; i < p->cols; i++) wr32(p->data + 4 * i, conv(MEM32(v + 4 * i), FXT_FLOAT, p->type));
}
static void vec_get(const struct fx_param *p, uint32_t v) {
    if (p->type == FXT_INT && p->bytes == 4) {
        uint32_t n = rd32(p->data);
        MEM32(v) = f2u((float)(n >> 16 & 0xFF) / 255.0f), MEM32(v + 4) = f2u((float)(n >> 8 & 0xFF) / 255.0f);
        MEM32(v + 8) = f2u((float)(n & 0xFF) / 255.0f), MEM32(v + 12) = f2u((float)(n >> 24) / 255.0f);
        return;
    }
    for (uint32_t i = 0; i < 4; i++) MEM32(v + 4 * i) = i < p->cols ? conv(rd32(p->data + 4 * i), p->type, FXT_FLOAT) : 0;
}
static void x_SetVector(void) {                     /* (this, h, D3DXVECTOR4*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!vec_like(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    vec_set(p, A32(2));
    dirty(p);
    RET(D3D_OK, 3);
}
static void x_GetVector(void) {                     /* (this, h, D3DXVECTOR4*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!vec_like(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    vec_get(p, A32(2));
    RET(D3D_OK, 3);
}
static void x_SetVectorArray(void) {                /* (this, h, D3DXVECTOR4*, count) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t v = A32(2), n = A32(3);
    if (!p || p->cls != FXC_VECTOR || !numeric(p) || !p->elements || n > p->elements || (!v && n))
        RET(D3DERR_INVALIDCALL, 4);
    for (uint32_t i = 0; i < n; i++) vec_set(&p->members[i], v + 16 * i);
    dirty(p);
    RET(D3D_OK, 4);
}
static void x_GetVectorArray(void) {                /* (this, h, D3DXVECTOR4*, count) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t v = A32(2), n = A32(3);
    if (!n && p) RET(D3D_OK, 4);
    if (!p || p->cls != FXC_VECTOR || !numeric(p) || !p->elements || n > p->elements || !v) RET(D3DERR_INVALIDCALL, 4);
    for (uint32_t i = 0; i < n; i++) vec_get(&p->members[i], v + 16 * i);
    RET(D3D_OK, 4);
}

static void set_matrix(int transpose) {             /* (this, h, D3DXMATRIX*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!matrix(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    mat_set(p, A32(2), transpose);
    dirty(p);
    RET(D3D_OK, 3);
}
static void get_matrix(int transpose) {             /* (this, h, D3DXMATRIX*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!matrix(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    mat_get(p, A32(2), transpose);
    RET(D3D_OK, 3);
}
/* the arrays: by value (consecutive matrices) or by pointers */
static void set_matrices(int transpose, int pointers) {   /* (this, h, matrices, count) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t m = A32(2), n = A32(3);
    if (!matrix(p) || !p->elements || n > p->elements || (!m && n)) RET(D3DERR_INVALIDCALL, 4);
    for (uint32_t i = 0; i < n; i++) mat_set(&p->members[i], pointers ? MEM32(m + 4 * i) : m + 64 * i, transpose);
    dirty(p);
    RET(D3D_OK, 4);
}
static void get_matrices(int transpose, int pointers) {   /* (this, h, matrices, count) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t m = A32(2), n = A32(3);
    if (!n && p) RET(D3D_OK, 4);
    if (!matrix(p) || !p->elements || n > p->elements || !m) RET(D3DERR_INVALIDCALL, 4);
    for (uint32_t i = 0; i < n; i++) mat_get(&p->members[i], pointers ? MEM32(m + 4 * i) : m + 64 * i, transpose);
    RET(D3D_OK, 4);
}
static void x_SetMatrix(void) { set_matrix(0); }
static void x_GetMatrix(void) { get_matrix(0); }
static void x_SetMatrixTranspose(void) { set_matrix(1); }
static void x_GetMatrixTranspose(void) { get_matrix(1); }
static void x_SetMatrixArray(void) { set_matrices(0, 0); }
static void x_GetMatrixArray(void) { get_matrices(0, 0); }
static void x_SetMatrixPointerArray(void) { set_matrices(0, 1); }
static void x_GetMatrixPointerArray(void) { get_matrices(0, 1); }
static void x_SetMatrixTransposeArray(void) { set_matrices(1, 0); }
static void x_GetMatrixTransposeArray(void) { get_matrices(1, 0); }
static void x_SetMatrixTransposePointerArray(void) { set_matrices(1, 1); }
static void x_GetMatrixTransposePointerArray(void) { get_matrices(1, 1); }

static void x_SetString(void) {                     /* (this, h, LPCSTR) */
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    if (!p || p->cls != FXC_OBJECT || p->type != FXT_STRING || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    uint32_t s = hle_guest_strdup(ASTR(2));
    e->strings = realloc(e->strings, sizeof *e->strings * (e->nstrings + 1));
    e->strings[e->nstrings++] = s;
    wr32(p->data, s);
    dirty(p);
    RET(D3D_OK, 3);
}
static void x_GetString(void) {                     /* (this, h, LPCSTR*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!p || p->cls != FXC_OBJECT || p->type != FXT_STRING || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = rd32(p->data);
    RET(D3D_OK, 3);
}
static void x_SetTexture(void) {                    /* (this, h, IDirect3DBaseTexture9*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t t = A32(2);
    if (!p || p->cls != FXC_OBJECT || !FX_IS_TEXTURE(p->type) || p->elements) RET(D3DERR_INVALIDCALL, 3);
    uint32_t old = rd32(p->data);
    if (old != t) {
        com_addref(t);
        wr32(p->data, t);
        com_release(old);
        dirty(p);
    }
    RET(D3D_OK, 3);
}
static void x_GetTexture(void) {                    /* (this, h, IDirect3DBaseTexture9**) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!p || p->cls != FXC_OBJECT || !FX_IS_TEXTURE(p->type) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    uint32_t t = rd32(p->data);
    com_addref(t);
    MEM32(A32(2)) = t;
    RET(D3D_OK, 3);
}
static void x_SetArrayRange(void) {                 /* (this, h, start, end): recorded; the upload is Task 6's */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    uint32_t lo = A32(2), hi = A32(3);
    if (!p || !p->elements || lo > hi || hi > p->elements) RET(D3DERR_INVALIDCALL, 4);
    p->array_lo = lo, p->array_hi = hi;
    RET(D3D_OK, 4);
}

static void x_GetPool(void) {                       /* (this, ID3DXEffectPool**) */
    struct fx_com *c = d3d_host(A32(0), D3DK_EFFECT);
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    if (c->pool) d3d_addref(c->pool);
    MEM32(A32(1)) = c->pool;
    RET(D3D_OK, 2);
}
static void x_GetDevice(void) {                     /* (this, IDirect3DDevice9**) */
    struct fx_com *c = d3d_host(A32(0), D3DK_EFFECT);
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(c->device);
    MEM32(A32(1)) = c->device;
    RET(D3D_OK, 2);
}

/* ---- pools ---- */

static void pool_destroy(struct d3d_obj *o) {
    struct fx_pool *pl = (struct fx_pool *)o;
    for (struct fx_shared *s = pl->list, *n; s; s = n) {
        n = s->next;
        for (uint32_t i = 0; i < s->nobj; i++) com_release(rd32(s->data + s->objoff[i]));
        free(s->name), free(s->sig), free(s->data), free(s->objoff), free(s);
    }
    free(pl);
}
static void x_D3DXCreateEffectPool(void) {          /* (ID3DXEffectPool**) */
    if (!A32(0)) RET(D3DERR_INVALIDCALL, 1);
    struct fx_pool *pl = calloc(1, sizeof *pl);
    pl->base.destroy = pool_destroy;
    MEM32(A32(0)) = d3d_new(D3DK_EFFECTPOOL, "ID3DXEffectPool", &pl->base);
    RET(D3D_OK, 1);
}
static struct fx_shared *pool_find(struct fx_pool *pl, const char *name) {
    for (struct fx_shared *s = pl->list; s; s = s->next)
        if (!strcmp(s->name, name)) return s;
    return NULL;
}
static struct fx_shared *g_new_shared;
static void note_objects(struct fx_param *leaf, void *ctx) {
    struct fx_param *top = ctx;
    if (leaf->cls != FXC_OBJECT || FX_IS_SAMPLER(leaf->type)) return;
    if (!FX_IS_TEXTURE(leaf->type))
        d3d_fail("D3DXCreateEffect: shared parameter '%s' holds a %s, which effect pools do not share here",
                 top->name, leaf->type == FXT_STRING ? "string" : "shader");
    struct fx_shared *s = g_new_shared;
    s->objoff = realloc(s->objoff, sizeof *s->objoff * (s->nobj + 1));
    s->objoff[s->nobj++] = (uint32_t)(leaf->data - top->data);
}
/* bind e's shared parameters to the pool's values: 0, or -1 with msg when a type differs */
static int pool_bind(struct fx_pool *pl, struct fx_effect *e, char *msg, size_t n) {
    for (uint32_t i = 0; i < e->nparams; i++) {
        struct fx_param *p = &e->params[i];
        if (!(p->flags & FX_SHARED) || !p->name) continue;
        struct fx_shared *s = pool_find(pl, p->name);
        if (!s) continue;
        char *sig = fx_signature(p);
        int same = !strcmp(sig, s->sig);
        free(sig);
        if (!same) {
            snprintf(msg, n, "shared parameter '%s' does not match the type it has in the effect pool", p->name);
            return -1;
        }
    }
    for (uint32_t i = 0; i < e->nparams; i++) {
        struct fx_param *p = &e->params[i];
        if (!(p->flags & FX_SHARED) || !p->name) continue;
        struct fx_shared *s = pool_find(pl, p->name);
        if (!s) {                                   /* the first effect to share it: its default is the value */
            s = calloc(1, sizeof *s);
            s->name = strdup(p->name), s->sig = fx_signature(p), s->bytes = p->bytes;
            s->data = calloc(1, p->bytes ? p->bytes : 1);
            memcpy(s->data, p->data, p->bytes);
            g_new_shared = s;
            fx_leaves(p, note_objects, p);
            s->next = pl->list, pl->list = s;
        }
        free(p->owned);
        p->owned = NULL;
        fx_bind(p, s->data, &s->serial);
    }
    return 0;
}

/* ---- the effect ---- */

static void effect_destroy(struct d3d_obj *o) {
    struct fx_com *c = (struct fx_com *)o;
    struct fx_effect *e = &c->fx;
    for (uint32_t i = 0; i < e->nparams; i++)
        if (e->params[i].owned) fx_leaves(&e->params[i], release_leaf, NULL);
    for (uint32_t i = 0; i < e->nobjects; i++) com_release(e->objects[i].guest);
    fx_free(e);
    d3d_release(c->device);
    if (c->pool) d3d_release(c->pool);
    free(c);
}
/* a clone's values: the source's current ones (its strings copied, its textures referenced again) */
static struct fx_effect *g_clone;
static void clone_leaf(struct fx_param *leaf, void *ctx) {
    (void)ctx;
    if (leaf->cls != FXC_OBJECT || !leaf->data) return;
    uint32_t v = rd32(leaf->data);
    if (holds_ref(leaf)) com_addref(v);
    else if (leaf->type == FXT_STRING && v) {
        uint32_t s = hle_guest_strdup((const char *)hle_ptr(v));
        g_clone->strings = realloc(g_clone->strings, sizeof *g_clone->strings * (g_clone->nstrings + 1));
        g_clone->strings[g_clone->nstrings++] = s;
        wr32(leaf->data, s);
    }
}
static void x_CloneEffect(void) {                   /* (this, IDirect3DDevice9*, ID3DXEffect**) */
    struct fx_com *c = d3d_host(A32(0), D3DK_EFFECT);
    uint32_t dev = A32(1), out = A32(2);
    if (!dev || !out || (c->flags & D3DXFX_NOT_CLONEABLE)) RET(D3DERR_INVALIDCALL, 3);
    d3d_host(dev, D3DK_DEVICE);
    struct fx_com *k = calloc(1, sizeof *k);
    char err[256];
    if (fx_parse(c->fx.raw, c->fx.glen, &k->fx, err, sizeof err)) d3d_fail("CloneEffect: %s", err);
    if (c->fx.pool && pool_bind(c->fx.pool, &k->fx, err, sizeof err)) d3d_fail("CloneEffect: %s", err);
    g_clone = &k->fx;
    for (uint32_t i = 0; i < k->fx.nparams; i++) {
        struct fx_param *p = &k->fx.params[i];
        if (!p->owned) continue;                    /* shared: the pool's value already */
        memcpy(p->owned, c->fx.params[i].data, p->bytes);
        p->array_lo = c->fx.params[i].array_lo, p->array_hi = c->fx.params[i].array_hi;
        fx_leaves(p, clone_leaf, NULL);
    }
    k->fx.pool = c->fx.pool;
    k->base.destroy = effect_destroy;
    k->device = dev, k->pool = c->pool, k->flags = c->flags;
    d3d_addref(dev);
    if (k->pool) d3d_addref(k->pool);
    MEM32(out) = d3d_new(D3DK_EFFECT, "ID3DXEffect", &k->base);
    RET(D3D_OK, 3);
}

static uint32_t errors(const char *msg) {
    uint32_t n = (uint32_t)strlen(msg) + 1, b = d3dx_buffer_new(n);
    memcpy(hle_ptr(d3dx_buffer_ptr(b)), msg, n);
    return b;
}
/* (device, data, len, defines, include, flags, pool, ID3DXEffect**, ID3DXBuffer** errors) */
static void x_D3DXCreateEffect(void) {
    uint32_t dev = A32(0), len = A32(2), pool = A32(6), out = A32(7), errs = A32(8);
    const uint8_t *data = APTR(1);
    if (errs) MEM32(errs) = 0;
    if (out) MEM32(out) = 0;
    if (!dev || !data || !len || !out) RET(D3DERR_INVALIDCALL, 9);
    d3d_host(dev, D3DK_DEVICE);
    uint32_t magic = 0;
    memcpy(&magic, data, len < 4 ? len : 4);
    if (len < 4 || magic != FX_MAGIC) d3d_later("D3DXCreateEffect (HLSL source)");
    struct fx_pool *pl = pool ? d3d_host(pool, D3DK_EFFECTPOOL) : NULL;
    struct fx_com *c = calloc(1, sizeof *c);
    char err[256];
    if (fx_parse(data, len, &c->fx, err, sizeof err)) d3d_fail("D3DXCreateEffect: %s", err);
    if (pl && pool_bind(pl, &c->fx, err, sizeof err)) {
        fx_free(&c->fx);
        free(c);
        if (errs) MEM32(errs) = errors(err);
        RET(D3DERR_INVALIDCALL, 9);
    }
    c->fx.pool = pl;
    c->base.destroy = effect_destroy;
    c->device = dev, c->pool = pool, c->flags = A32(5);
    d3d_addref(dev);
    if (pool) d3d_addref(pool);
    MEM32(out) = d3d_new(D3DK_EFFECT, "ID3DXEffect", &c->base);
    RET(D3D_OK, 9);
}

static const win32hle_shim g_impls[] = {
    { "ID3DXEffect::GetDesc", x_GetDesc }, { "ID3DXEffect::GetParameterDesc", x_GetParameterDesc },
    { "ID3DXEffect::GetTechniqueDesc", x_GetTechniqueDesc }, { "ID3DXEffect::GetPassDesc", x_GetPassDesc },
    { "ID3DXEffect::GetFunctionDesc", x_GetFunctionDesc }, { "ID3DXEffect::GetParameter", x_GetParameter },
    { "ID3DXEffect::GetParameterByName", x_GetParameterByName },
    { "ID3DXEffect::GetParameterBySemantic", x_GetParameterBySemantic },
    { "ID3DXEffect::GetParameterElement", x_GetParameterElement }, { "ID3DXEffect::GetTechnique", x_GetTechnique },
    { "ID3DXEffect::GetTechniqueByName", x_GetTechniqueByName }, { "ID3DXEffect::GetPass", x_GetPass },
    { "ID3DXEffect::GetPassByName", x_GetPassByName }, { "ID3DXEffect::GetFunction", x_GetFunction },
    { "ID3DXEffect::GetFunctionByName", x_GetFunctionByName }, { "ID3DXEffect::GetAnnotation", x_GetAnnotation },
    { "ID3DXEffect::GetAnnotationByName", x_GetAnnotationByName }, { "ID3DXEffect::SetValue", x_SetValue },
    { "ID3DXEffect::GetValue", x_GetValue }, { "ID3DXEffect::SetBool", x_SetBool }, { "ID3DXEffect::GetBool", x_GetBool },
    { "ID3DXEffect::SetBoolArray", x_SetBoolArray }, { "ID3DXEffect::GetBoolArray", x_GetBoolArray },
    { "ID3DXEffect::SetInt", x_SetInt }, { "ID3DXEffect::GetInt", x_GetInt },
    { "ID3DXEffect::SetIntArray", x_SetIntArray }, { "ID3DXEffect::GetIntArray", x_GetIntArray },
    { "ID3DXEffect::SetFloat", x_SetFloat }, { "ID3DXEffect::GetFloat", x_GetFloat },
    { "ID3DXEffect::SetFloatArray", x_SetFloatArray }, { "ID3DXEffect::GetFloatArray", x_GetFloatArray },
    { "ID3DXEffect::SetVector", x_SetVector }, { "ID3DXEffect::GetVector", x_GetVector },
    { "ID3DXEffect::SetVectorArray", x_SetVectorArray }, { "ID3DXEffect::GetVectorArray", x_GetVectorArray },
    { "ID3DXEffect::SetMatrix", x_SetMatrix }, { "ID3DXEffect::GetMatrix", x_GetMatrix },
    { "ID3DXEffect::SetMatrixArray", x_SetMatrixArray }, { "ID3DXEffect::GetMatrixArray", x_GetMatrixArray },
    { "ID3DXEffect::SetMatrixPointerArray", x_SetMatrixPointerArray },
    { "ID3DXEffect::GetMatrixPointerArray", x_GetMatrixPointerArray },
    { "ID3DXEffect::SetMatrixTranspose", x_SetMatrixTranspose },
    { "ID3DXEffect::GetMatrixTranspose", x_GetMatrixTranspose },
    { "ID3DXEffect::SetMatrixTransposeArray", x_SetMatrixTransposeArray },
    { "ID3DXEffect::GetMatrixTransposeArray", x_GetMatrixTransposeArray },
    { "ID3DXEffect::SetMatrixTransposePointerArray", x_SetMatrixTransposePointerArray },
    { "ID3DXEffect::GetMatrixTransposePointerArray", x_GetMatrixTransposePointerArray },
    { "ID3DXEffect::SetString", x_SetString }, { "ID3DXEffect::GetString", x_GetString },
    { "ID3DXEffect::SetTexture", x_SetTexture }, { "ID3DXEffect::GetTexture", x_GetTexture },
    { "ID3DXEffect::SetArrayRange", x_SetArrayRange }, { "ID3DXEffect::GetPool", x_GetPool },
    { "ID3DXEffect::GetDevice", x_GetDevice }, { "ID3DXEffect::CloneEffect", x_CloneEffect },
    { 0, 0 } };
static const win32hle_shim g_exports[] = {
    { "D3DXCreateEffect", x_D3DXCreateEffect }, { "D3DXCreateEffectPool", x_D3DXCreateEffectPool }, { 0, 0 } };
void fx_effect_register(void) { d3d_impls(g_impls); win32hle_register(g_exports); }
