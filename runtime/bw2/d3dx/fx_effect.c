/*
 * fx_effect.c - ID3DXEffect's ID3DXBaseEffect methods (descriptions, handles
 * and parameter values), D3DXCreateEffect and D3DXCreateEffectPool (plan 5
 * Task 5), and CloneEffect, which the game calls on every effect it loads.
 *
 * Values follow D3DX's conversions: numbers convert between bool, int and
 * float on the way in and out (a float set on an int truncates, a bool reads
 * 0/1); an int set on a float3/float4 vector is a D3DCOLOR and the reverse;
 * matrices are stored as declared (rows or columns) and converted at
 * Set/Get; textures (and shaders) in values hold a reference. A call on the
 * wrong kind of parameter gives D3DERR_INVALIDCALL.
 *
 * Task 6: the technique and pass methods (SetTechnique, ValidateTechnique,
 * Begin/BeginPass/CommitChanges/EndPass/End ...), whose work is fx_states.c's.
 * SetStateManager stays unimplemented: it stops loudly, naming itself.
 *
 * Plan 10 Task 4 (Ruling 10): parameter blocks. While a block records, every
 * setter writes through fx_dest into a new record of the block, and the
 * parameter's live value, its dirty serial and its references stay as they
 * were; ApplyParameterBlock writes the records in order as SetValue would.
 *
 * A pool holds one value per shared parameter name: effects created with the
 * pool bind their shared parameters to it, and a type that differs from the
 * pool's fails D3DXCreateEffect with D3DERR_INVALIDCALL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
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

/* ---- parameter blocks: where a setter writes (plan 10 Task 4, Ruling 10) ---- */

/* the references held by buf, a value laid out as p's: taken (add) or dropped */
struct refs_ctx { const struct fx_param *p; const uint8_t *buf; int add; };
static void refs_leaf(struct fx_param *leaf, void *ctx) {
    const struct refs_ctx *c = ctx;
    if (!holds_ref(leaf) || !leaf->data) return;
    uint32_t v = rd32(c->buf + (leaf->data - c->p->data));
    if (c->add) com_addref(v);
    else com_release(v);
}
static void value_refs(struct fx_param *p, const uint8_t *buf, int add) {
    struct refs_ctx c = { p, buf, add };
    fx_leaves(p, refs_leaf, &c);
}
uint8_t *fx_dest(struct fx_effect *e, struct fx_param *p) {
    struct fx_block *b = e->recording;
    if (!b) return p->data;
    const uint8_t *from = p->data;                   /* the newest record of p in this block, else its live value */
    for (uint32_t i = b->n; i-- > 0;)
        if (b->recs[i].p == p) { from = b->recs[i].data; break; }
    if (b->n == b->cap) {
        b->cap = b->cap ? 2 * b->cap : 16;
        b->recs = realloc(b->recs, sizeof *b->recs * b->cap);
    }
    uint8_t *d = malloc(p->bytes ? p->bytes : 1);
    memcpy(d, from, p->bytes);
    value_refs(p, d, 1);                             /* the record's own references; the setter swaps on it */
    b->recs[b->n++] = (struct fx_record){ p, d };
    return d;
}
/* a set's dirty mark, which a recording skips (Apply marks it) */
static void touch(struct fx_effect *e, struct fx_param *p) { if (!e->recording) dirty(p); }
static void block_free(struct fx_block *b) {
    for (uint32_t i = 0; i < b->n; i++) value_refs(b->recs[i].p, b->recs[i].data, 0), free(b->recs[i].data);
    free(b->recs);
    if (b->handle) hle_free(hle_ptr(b->handle));
    free(b);
}

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
static void mat_set(const struct fx_param *p, uint8_t *dst, uint32_t m, int transpose) {
    for (uint32_t r = 0; r < p->rows; r++)
        for (uint32_t c = 0; c < p->cols; c++) {
            uint32_t v = MEM32(m + 4 * (transpose ? 4 * c + r : 4 * r + c));
            wr32(dst + 4 * midx(p, r, c), conv(v, FXT_FLOAT, p->type));
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

/* src and dst laid out as p's value: each reference src names is taken, each one dst held is dropped */
struct swap_ctx { const struct fx_param *p; const uint8_t *src, *dst; };
static void swap_refs(struct fx_param *leaf, void *ctx) {
    const struct swap_ctx *c = ctx;
    if (!holds_ref(leaf) || !leaf->data) return;
    size_t off = (size_t)(leaf->data - c->p->data);
    uint32_t nv = rd32(c->src + off), ov = rd32(c->dst + off);
    if (nv != ov) com_addref(nv), com_release(ov);
}
/* SetValue's work: p's whole value from src, where fx_dest says (ApplyParameterBlock's too) */
static void set_value(struct fx_effect *e, struct fx_param *p, const uint8_t *src) {
    uint8_t *dst = fx_dest(e, p);
    struct swap_ctx c = { p, src, dst };
    fx_leaves(p, swap_refs, &c);
    memcpy(dst, src, p->bytes);
    touch(e, p);
}
static void x_SetValue(void) {                      /* (this, h, data, bytes) */
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    const uint8_t *src = APTR(2);
    if (!p || !src || !p->bytes || A32(3) < p->bytes) RET(D3DERR_INVALIDCALL, 4);
    set_value(e, p, src);
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
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    if (!scalar1(p)) RET(D3DERR_INVALIDCALL, 3);
    wr32(fx_dest(e, p), conv(A32(2), FXT_BOOL, p->type));
    touch(e, p);
    RET(D3D_OK, 3);
}
static void x_GetBool(void) {                       /* (this, h, BOOL*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!scalar1(p) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = conv(rd32(p->data), p->type, FXT_BOOL);
    RET(D3D_OK, 3);
}
static void x_SetInt(void) {                        /* (this, h, INT) */
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    uint32_t n = A32(2);
    if (!scalar1(p) && !colour_vec(p)) RET(D3DERR_INVALIDCALL, 3);
    uint8_t *d = fx_dest(e, p);
    if (scalar1(p)) wr32(d, conv(n, FXT_INT, p->type));
    else {                                          /* a D3DCOLOR: r, g, b (and a) */
        wr32(d, f2u((float)(n >> 16 & 0xFF) / 255.0f));
        wr32(d + 4, f2u((float)(n >> 8 & 0xFF) / 255.0f));
        wr32(d + 8, f2u((float)(n & 0xFF) / 255.0f));
        if (p->cols > 3) wr32(d + 12, f2u((float)(n >> 24) / 255.0f));
    }
    touch(e, p);
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
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    if (!scalar1(p)) RET(D3DERR_INVALIDCALL, 3);
    wr32(fx_dest(e, p), conv(A32(2), FXT_FLOAT, p->type));
    touch(e, p);
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
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    uint32_t src = A32(2), n = A32(3);
    if (!numeric(p) || (!src && n)) RET(D3DERR_INVALIDCALL, 4);
    if (n > p->bytes / 4) n = p->bytes / 4;
    uint8_t *d = fx_dest(e, p);
    for (uint32_t i = 0; i < n; i++) wr32(d + 4 * i, conv(MEM32(src + 4 * i), from, p->type));
    touch(e, p);
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
static void vec_set(const struct fx_param *p, uint8_t *dst, uint32_t v) {
    if (p->type == FXT_INT && p->bytes == 4) {      /* an int vector of one: a D3DCOLOR */
        uint32_t n = (uint32_t)(sat(gfl(v + 8)) * 255.0f) + ((uint32_t)(sat(gfl(v + 4)) * 255.0f) << 8)
                   + ((uint32_t)(sat(gfl(v)) * 255.0f) << 16) + ((uint32_t)(sat(gfl(v + 12)) * 255.0f) << 24);
        wr32(dst, n);
        return;
    }
    for (uint32_t i = 0; i < p->cols; i++) wr32(dst + 4 * i, conv(MEM32(v + 4 * i), FXT_FLOAT, p->type));
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
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    if (!vec_like(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    vec_set(p, fx_dest(e, p), A32(2));
    touch(e, p);
    RET(D3D_OK, 3);
}
static void x_GetVector(void) {                     /* (this, h, D3DXVECTOR4*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!vec_like(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    vec_get(p, A32(2));
    RET(D3D_OK, 3);
}
static void x_SetVectorArray(void) {                /* (this, h, D3DXVECTOR4*, count) */
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    uint32_t v = A32(2), n = A32(3);
    if (!p || p->cls != FXC_VECTOR || !numeric(p) || !p->elements || n > p->elements || (!v && n))
        RET(D3DERR_INVALIDCALL, 4);
    uint8_t *d = fx_dest(e, p);
    for (uint32_t i = 0; i < n; i++) vec_set(&p->members[i], d + (p->members[i].data - p->data), v + 16 * i);
    touch(e, p);
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
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    if (!matrix(p) || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    mat_set(p, fx_dest(e, p), A32(2), transpose);
    touch(e, p);
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
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    uint32_t m = A32(2), n = A32(3);
    if (!matrix(p) || !p->elements || n > p->elements || (!m && n)) RET(D3DERR_INVALIDCALL, 4);
    uint8_t *d = fx_dest(e, p);
    for (uint32_t i = 0; i < n; i++)
        mat_set(&p->members[i], d + (p->members[i].data - p->data), pointers ? MEM32(m + 4 * i) : m + 64 * i,
                transpose);
    touch(e, p);
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
    wr32(fx_dest(e, p), s);
    touch(e, p);
    RET(D3D_OK, 3);
}
static void x_GetString(void) {                     /* (this, h, LPCSTR*) */
    struct fx_param *p = fx_param_of(FX(0), A32(1));
    if (!p || p->cls != FXC_OBJECT || p->type != FXT_STRING || p->elements || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = rd32(p->data);
    RET(D3D_OK, 3);
}
static void x_SetTexture(void) {                    /* (this, h, IDirect3DBaseTexture9*) */
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    uint32_t t = A32(2);
    if (!p || p->cls != FXC_OBJECT || !FX_IS_TEXTURE(p->type) || p->elements) RET(D3DERR_INVALIDCALL, 3);
    uint8_t *d = fx_dest(e, p);                     /* the live slot, or a record (which holds its own reference) */
    uint32_t old = rd32(d);
    if (old != t) {
        com_addref(t);
        wr32(d, t);
        com_release(old);
        touch(e, p);
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
/* ---- techniques and passes (Task 6; the work is fx_states.c's) ---- */

#define S_FALSE 1u
static void x_SetTechnique(void) {                  /* (this, hTechnique) */
    struct fx_effect *e = FX(0);
    struct fx_technique *t = fx_tech_of(e, A32(1));
    if (!t) RET(D3DERR_INVALIDCALL, 2);
    e->cur = t;
    RET(D3D_OK, 2);
}
static void x_GetCurrentTechnique(void) {           /* (this) */
    struct fx_effect *e = FX(0);
    RET(e->cur ? e->cur->handle : 0, 1);
}
static void x_ValidateTechnique(void) {             /* (this, hTechnique) */
    struct fx_effect *e = FX(0);
    struct fx_technique *t = fx_tech_of(e, A32(1));
    RET(t && fx_tech_valid(e, t) ? D3D_OK : D3DERR_INVALIDCALL, 2);
}
static void x_FindNextValidTechnique(void) {        /* (this, hTechnique or NULL, D3DXHANDLE*) */
    struct fx_effect *e = FX(0);
    uint32_t out = A32(2), i = 0;
    if (!out) RET(D3DERR_INVALIDCALL, 3);
    if (A32(1)) {
        struct fx_technique *t = fx_tech_of(e, A32(1));
        if (!t) RET(D3DERR_INVALIDCALL, 3);
        i = (uint32_t)(t - e->techs) + 1;
    }
    for (; i < e->ntechs; i++)
        if (fx_tech_valid(e, &e->techs[i])) { MEM32(out) = e->techs[i].handle; RET(D3D_OK, 3); }
    MEM32(out) = 0;
    RET(S_FALSE, 3);
}
static void x_IsParameterUsed(void) {               /* (this, hParameter, hTechnique): BOOL */
    struct fx_effect *e = FX(0);
    struct fx_param *p = fx_param_of(e, A32(1));
    struct fx_technique *t = fx_tech_of(e, A32(2));
    RET(p && t && fx_param_used(e, p, t) ? 1u : 0u, 3);
}
static void end_effect(struct fx_effect *e) {      /* End's work: the saved state back */
    if (e->saved) {
        uint32_t a[1] = { e->saved };
        hle_call_guest(MEM32(MEM32(e->saved) + 4u * 5u), 1, a);   /* IDirect3DStateBlock9::Apply */
        d3d_release(e->saved);
        e->saved = 0;
    }
    e->begun = 0, e->pass = NULL;
}
static void x_Begin(void) {                         /* (this, UINT* passes, flags) */
    struct fx_effect *e = FX(0);
    uint32_t np = A32(1), flags = A32(2);
    if (!e->cur) RET(D3DERR_INVALIDCALL, 3);
    if (e->begun) end_effect(e);
    if (np) MEM32(np) = e->cur->npasses;
    e->saved = flags & 1u ? 0 : fx_begin(e, flags);    /* D3DXFX_DONOTSAVESTATE */
    e->begun = 1, e->begin_flags = flags, e->pass = NULL;
    RET(D3D_OK, 3);
}
static void x_BeginPass(void) {                     /* (this, pass) */
    struct fx_effect *e = FX(0);
    uint32_t i = A32(1);
    if (!e->begun || !e->cur || i >= e->cur->npasses) RET(D3DERR_INVALIDCALL, 2);
    e->pass = &e->cur->passes[i];
    fx_apply_pass(e, e->pass, 1);
    RET(D3D_OK, 2);
}
static void x_CommitChanges(void) {                 /* (this) */
    struct fx_effect *e = FX(0);
    if (e->pass) fx_apply_pass(e, e->pass, 0);
    RET(D3D_OK, 1);
}
static void x_EndPass(void) {                       /* (this) */
    struct fx_effect *e = FX(0);
    if (!e->pass) RET(D3DERR_INVALIDCALL, 1);
    e->pass = NULL;
    RET(D3D_OK, 1);
}
static void x_End(void) {                           /* (this) */
    struct fx_effect *e = FX(0);
    if (e->begun) end_effect(e);
    RET(D3D_OK, 1);
}
static void x_OnLostDevice(void) { (void)FX(0); RET(D3D_OK, 1); }
static void x_OnResetDevice(void) { (void)FX(0); RET(D3D_OK, 1); }
static void x_GetStateManager(void) {               /* (this, ID3DXEffectStateManager**): none can be set */
    (void)FX(0);
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = 0;
    RET(D3D_OK, 2);
}

/* ---- parameter blocks (plan 10 Task 4, Ruling 10; misuse the game has not shown stops, naming the method) ---- */

static void x_BeginParameterBlock(void) {           /* (this) */
    struct fx_effect *e = FX(0);
    if (e->recording) d3d_fail("ID3DXEffect::BeginParameterBlock while a block is recording (a nested block)");
    e->recording = calloc(1, sizeof *e->recording);
    RET(D3D_OK, 1);
}
static void x_EndParameterBlock(void) {             /* (this): the block's handle */
    struct fx_effect *e = FX(0);
    struct fx_block *b = e->recording;
    if (!b) d3d_fail("ID3DXEffect::EndParameterBlock with no block recording");
    uint8_t *slot = hle_alloc(4);                   /* a guest dword of its own: no parameter's handle */
    memset(slot, 0, 4);
    b->handle = hle_va(slot);
    b->next = e->blocks, e->blocks = b, e->recording = NULL;
    RET(b->handle, 1);
}
static void x_ApplyParameterBlock(void) {           /* (this, hParameterBlock) */
    struct fx_effect *e = FX(0);
    uint32_t h = A32(1);
    struct fx_block *b = e->blocks;
    while (b && b->handle != h) b = b->next;
    if (!b) d3d_fail("ID3DXEffect::ApplyParameterBlock of 0x%08X, which is not a parameter block of this effect", h);
    if (!b->n) RET(D3DERR_INVALIDCALL, 2);         /* an empty block, as Wine's d3dx9 answers */
    for (uint32_t i = 0; i < b->n; i++) set_value(e, b->recs[i].p, b->recs[i].data);
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
    if (e->saved) d3d_release(e->saved);
    if (e->recording) block_free(e->recording);
    for (struct fx_block *b = e->blocks, *n; b; b = n) n = b->next, block_free(b);
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
    k->fx.device = dev, k->fx.cur = k->fx.ntechs ? &k->fx.techs[0] : NULL;
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
/* An effect from fx_2_0 bytes (the game's own, or the lifted compiler's), with the pool's shared values bound:
 * D3D_OK and *out, or D3DERR_INVALIDCALL and the reason in *errs (when errs) for a shared type that differs. */
static uint32_t build_effect(uint32_t dev, const uint8_t *data, uint32_t len, uint32_t pool, uint32_t flags,
                             uint32_t out, uint32_t errs, uint32_t *ntechs) {
    struct fx_pool *pl = pool ? d3d_host(pool, D3DK_EFFECTPOOL) : NULL;
    struct fx_com *c = calloc(1, sizeof *c);
    char err[256];
    if (fx_parse(data, len, &c->fx, err, sizeof err)) d3d_fail("D3DXCreateEffect: %s", err);
    if (pl && pool_bind(pl, &c->fx, err, sizeof err)) {
        fx_free(&c->fx);
        free(c);
        if (errs) MEM32(errs) = errors(err);
        return D3DERR_INVALIDCALL;
    }
    c->fx.pool = pl;
    c->fx.device = dev, c->fx.cur = c->fx.ntechs ? &c->fx.techs[0] : NULL;
    c->base.destroy = effect_destroy;
    c->device = dev, c->pool = pool, c->flags = flags;
    d3d_addref(dev);
    if (pool) d3d_addref(pool);
    if (ntechs) *ntechs = c->fx.ntechs;
    MEM32(out) = d3d_new(D3DK_EFFECT, "ID3DXEffect", &c->base);
    return D3D_OK;
}

/* A guest COM method (the lifted DLL's objects): obj's vtable slot with obj and n more arguments. */
static uint32_t guest_method(uint32_t obj, uint32_t slot, int n, const uint32_t *args) {
    uint32_t a[8] = { obj };
    for (int i = 0; i < n; i++) a[1 + i] = args[i];
    return hle_call_guest(MEM32(MEM32(obj) + 4 * slot), n + 1, a);
}
/* ID3DXEffectCompiler (d3dx9_25): the 57 ID3DXBaseEffect slots, SetLiteral 57, GetLiteral 58, CompileEffect 59,
 * CompileShader 60. ID3DXBuffer: GetBufferPointer 3, GetBufferSize 4. */
enum { EC_COMPILEEFFECT = 59, BUF_POINTER = 3, BUF_SIZE = 4 };
#define FAILED_HR(hr) (((hr) & 0x80000000u) != 0)

/* HLSL text (plan 9, Ruling 5): the lifted D3DXCreateEffectCompiler and ID3DXEffectCompiler::CompileEffect make
 * fx_2_0 bytes, which take the binary path. A failing HRESULT comes back as it is, with no effect and the lifted
 * error buffer in *errs (released when errs is NULL); so does a successful compile's buffer of warnings. */
static uint32_t effect_from_text(uint32_t dev, uint32_t src, uint32_t len, uint32_t defines, uint32_t include,
                                 uint32_t flags, uint32_t pool, uint32_t out, uint32_t errs) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t slots = hle_va(hle_alloc(12));             /* the compiler, the messages, the fx_2_0 buffer */
    memset(hle_ptr(slots), 0, 12);
    uint32_t a[7] = { src, len, defines, include, flags, slots, slots + 4 };
    uint32_t hr = hle_call_guest(d3dx_lifted_proc("D3DXCreateEffectCompiler"), 7, a);
    uint32_t msgs = MEM32(slots + 4), fx = 0, fxlen = 0, ntechs = 0;
    if (!FAILED_HR(hr)) {
        uint32_t compiler = MEM32(slots);
        if (!compiler) d3d_fail("D3DXCreateEffect: D3DXCreateEffectCompiler gave 0x%08X and no compiler", hr);
        com_release(msgs);                              /* warnings of the parse; CompileEffect gives its own */
        MEM32(slots + 4) = 0;
        uint32_t c[3] = { flags, slots + 8, slots + 4 };
        hr = guest_method(compiler, EC_COMPILEEFFECT, 3, c);
        com_release(compiler);
        msgs = MEM32(slots + 4), fx = MEM32(slots + 8);
    }
    if (!FAILED_HR(hr)) {
        if (!fx) d3d_fail("D3DXCreateEffect: CompileEffect gave 0x%08X and no effect", hr);
        uint32_t p = guest_method(fx, BUF_POINTER, 0, NULL);
        fxlen = guest_method(fx, BUF_SIZE, 0, NULL);
        uint32_t magic = fxlen >= 4 ? MEM32(p) : 0;
        if (magic != FX_MAGIC)
            d3d_fail("D3DXCreateEffect: the lifted compiler made %u bytes that are not fx_2_0 (0x%08X)", fxlen, magic);
        hr = build_effect(dev, hle_ptr(p), fxlen, pool, flags, out, errs, &ntechs);
        com_release(fx);
        if (FAILED_HR(hr)) {                            /* a shared type differs: build_effect wrote the reason */
            com_release(msgs);
            msgs = 0;
        }
    }
    if (errs && !MEM32(errs)) MEM32(errs) = msgs;        /* (a differing shared type has put its own reason there) */
    else com_release(msgs);
    hle_free(hle_ptr(slots));
    if (d3dx_hlsl_log()) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        fprintf(stderr, "[bw2] hlsl effect: %u bytes -> 0x%08X, %u bytes, %u techniques, %ld ms\n", len, hr,
                FAILED_HR(hr) ? 0 : fxlen, ntechs, ms);
    }
    return hr;
}

/* (device, data, len, defines, include, flags, pool, ID3DXEffect**, ID3DXBuffer** errors) */
static void x_D3DXCreateEffect(void) {
    uint32_t dev = A32(0), src = A32(1), len = A32(2), defines = A32(3), include = A32(4), flags = A32(5);
    uint32_t pool = A32(6), out = A32(7), errs = A32(8);
    const uint8_t *data = APTR(1);
    if (errs) MEM32(errs) = 0;
    if (out) MEM32(out) = 0;
    if (!dev || !data || !len || !out) RET(D3DERR_INVALIDCALL, 9);
    d3d_host(dev, D3DK_DEVICE);
    if (pool) d3d_host(pool, D3DK_EFFECTPOOL);
    uint32_t magic = 0;
    memcpy(&magic, data, len < 4 ? len : 4);
    uint32_t hr = len >= 4 && magic == FX_MAGIC ? build_effect(dev, data, len, pool, flags, out, errs, NULL)
                                                : effect_from_text(dev, src, len, defines, include, flags, pool, out, errs);
    RET(hr, 9);
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
    { "ID3DXEffect::SetTechnique", x_SetTechnique }, { "ID3DXEffect::GetCurrentTechnique", x_GetCurrentTechnique },
    { "ID3DXEffect::ValidateTechnique", x_ValidateTechnique },
    { "ID3DXEffect::FindNextValidTechnique", x_FindNextValidTechnique },
    { "ID3DXEffect::IsParameterUsed", x_IsParameterUsed }, { "ID3DXEffect::Begin", x_Begin },
    { "ID3DXEffect::BeginPass", x_BeginPass }, { "ID3DXEffect::CommitChanges", x_CommitChanges },
    { "ID3DXEffect::EndPass", x_EndPass }, { "ID3DXEffect::End", x_End },
    { "ID3DXEffect::OnLostDevice", x_OnLostDevice }, { "ID3DXEffect::OnResetDevice", x_OnResetDevice },
    { "ID3DXEffect::GetStateManager", x_GetStateManager },
    { "ID3DXEffect::BeginParameterBlock", x_BeginParameterBlock },
    { "ID3DXEffect::EndParameterBlock", x_EndParameterBlock },
    { "ID3DXEffect::ApplyParameterBlock", x_ApplyParameterBlock },
    { 0, 0 } };
static const win32hle_shim g_exports[] = {
    { "D3DXCreateEffect", x_D3DXCreateEffect }, { "D3DXCreateEffectPool", x_D3DXCreateEffectPool }, { 0, 0 } };
void fx_effect_register(void) { d3d_impls(g_impls); win32hle_register(g_exports); }
