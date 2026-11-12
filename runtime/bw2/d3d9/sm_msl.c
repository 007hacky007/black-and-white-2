/*
 * sm_msl.c - D3D9 shader bytecode to Metal Shading Language (plan 5).
 * Task 3: the vertex side. Each vertex shader becomes one `vertex VOut vsh`
 * that writes the VOut every fragment function reads (ff_msl.c's emitter).
 * The register file is one struct; each label is a function taking it by
 * reference. Task 4: the pixel side, one `fragment FOut psh` reading that
 * VOut, with plan 4's fog, alpha test and sRGB write after the shader.
 * Semantics follow the D3D9 shader reference as pinned in the plan's Global
 * Constraints.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d_com.h"
#include "ff.h"
#include "sm.h"

struct sb { char *s; size_t n, cap; };
static void put(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct sb *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        b->cap = b->cap ? b->cap * 2 + (size_t)k : 16384;
        b->s = realloc(b->s, b->cap);
        if (!b->s) abort();
    }
}

/* What the translation is working on, for messages; a pixel shader's variant, and its texm3x2/texm3x3
 * rows so far. */
struct tr {
    struct sb *b;
    const struct sm_shader *s;
    const struct sm_ins *ins;
    char ver[16];
    const struct sm_ps_variant *pv;
    int npad;
};

_Noreturn static void unsupported(const struct tr *t, const char *what) {
    char nb[16];
    const struct sm_opinfo *oi = sm_opinfo(t->ins->op, t->s->version);
    snprintf(nb, sizeof nb, "op %u", t->ins->op);
    d3d_fail("%s: %s%s%s is not supported by the translator (token %u)", t->ver, oi ? oi->name : nb,
             what ? ": " : "", what ? what : "", t->ins->offset);
}

static const char XYZW[] = "xyzw";

/* Helpers every translated shader gets. */
static const char *SM_HELPERS =
    "struct SMK { int4 i[16]; uint b[16]; };\n"
    "static inline float4 sm_c(constant float4 *c, int i) {\n"
    "  float4 v = c[clamp(i, 0, 255)];\n"
    "  return uint(i) < 256u ? v : float4(0.0);\n}\n"
    "static inline float sm_rcp(float x) { return x == 0.0 ? INFINITY : 1.0 / x; }\n"
    "static inline float sm_rsq(float x) { x = abs(x); return x == 0.0 ? INFINITY : rsqrt(x); }\n"
    "static inline float sm_log(float x) { x = abs(x); return x == 0.0 ? -INFINITY : log2(x); }\n"
    "static inline float4 sm_logp11(float x) {\n"
    "  x = abs(x);\n"
    "  if (x == 0.0) return float4(-INFINITY, 1.0, -INFINITY, 1.0);\n"
    "  float e = floor(log2(x));\n"
    "  return float4(e, x / exp2(e), log2(x), 1.0);\n}\n"
    "static inline float4 sm_expp11(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "static inline float4 sm_lit(float4 s) {\n"
    "  float p = clamp(s.w, -128.0, 128.0);\n"
    "  return float4(1.0, max(s.x, 0.0), s.x > 0.0 && s.y > 0.0 ? pow(s.y, p) : 0.0, 1.0);\n}\n"
    "static inline float4 sm_nrm(float4 v) {\n"
    "  float l = dot(v.xyz, v.xyz);\n"
    "  return l > 0.0 ? v * rsqrt(l) : float4(0.0);\n}\n"
    "static inline float sm_pow(float x, float y) { return pow(abs(x), y); }\n"
    "static inline float4 sm_slt(float4 a, float4 b) { return select(float4(0.0), float4(1.0), a < b); }\n"
    "static inline float4 sm_sge(float4 a, float4 b) { return select(float4(0.0), float4(1.0), a >= b); }\n";

_Noreturn static void bad_reg(struct tr *t, const struct sm_reg *r, int dst) {
    char rb[32], msg[64];
    snprintf(msg, sizeof msg, "register %s as a %s", sm_reg_name(t->s->version, r, rb, sizeof rb),
             dst ? "destination" : "source");
    unsupported(t, msg);
}

/* A pixel shader's register: r# temporaries, v# colours, c# constants, t# texture registers (1.1-1.3:
 * read and written; later read only), oC# and oDepth. */
static void ps_reg(struct tr *t, const struct sm_reg *r, int dst) {
    struct sb *b = t->b;
    if (r->rel) unsupported(t, "relative addressing");
    switch (r->type) {
    case SM_R_TEMP: put(b, "s.r[%u]", r->index); return;
    case SM_R_INPUT: if (!dst) { put(b, "s.v[%u]", r->index); return; } break;
    case SM_R_CONST: if (!dst) { put(b, "c[%u]", r->index); return; } break;
    case SM_R_TEXTURE: if (!dst || t->s->major == 1) { put(b, "s.t[%u]", r->index); return; } break;
    case SM_R_COLOROUT: if (dst) { put(b, "s.oc[%u]", r->index); return; } break;
    case SM_R_DEPTHOUT: if (dst) { put(b, "s.od"); return; } break;
    default: break;
    }
    bad_reg(t, r, dst);
}

/* A register as an lvalue or base expression. */
static void reg_base(struct tr *t, const struct sm_reg *r, int dst) {
    struct sb *b = t->b;
    if (t->s->is_ps) { ps_reg(t, r, dst); return; }
    switch (r->type) {
    case SM_R_TEMP: put(b, "s.r[%u]", r->index); return;
    case SM_R_INPUT: if (!dst) { put(b, "s.v[%u]", r->index); return; } break;
    case SM_R_CONST:
        if (dst) break;
        if (r->rel) {
            if (r->rel_type == SM_R_ADDR) put(b, "sm_c(c, s.a.%c + %u)", XYZW[r->rel_comp & 3], r->index);
            else if (r->rel_type == SM_R_LOOP) put(b, "sm_c(c, s.aL + %u)", r->index);
            else unsupported(t, "relative addressing through that register");
        } else put(b, "c[%u]", r->index);
        return;
    case SM_R_ADDR: if (dst) { put(b, "s.a"); return; } break;
    case SM_R_RASTOUT:
        if (!dst) break;
        if (r->index == SM_RAST_POSITION) put(b, "s.opos");
        else if (r->index == SM_RAST_FOG) put(b, "s.ofog");
        else put(b, "s.opts");
        return;
    case SM_R_ATTROUT: if (dst) { put(b, "s.od[%u]", r->index); return; } break;
    case SM_R_TEXCRDOUT: if (dst) { put(b, "s.ot[%u]", r->index); return; } break;
    default: break;
    }
    bad_reg(t, r, dst);
}

/* Source k: n = 4 the swizzled float4; n = 1 the scalar a replicate swizzle picks (its last lane). */
static void src(struct tr *t, int k, int n) {
    const struct sm_reg *r = &t->ins->src[k];
    struct sb *b = t->b;
    static const char *pre[] = { [SM_SRC_NONE] = "(", [SM_SRC_NEG] = "(-", [SM_SRC_BIAS] = "((",
                                 [SM_SRC_BIASNEG] = "(-((", [SM_SRC_SIGN] = "(2.0 * ", [SM_SRC_SIGNNEG] = "(-2.0 * ",
                                 [SM_SRC_COMP] = "(1.0 - ", [SM_SRC_X2] = "(2.0 * ", [SM_SRC_X2NEG] = "(-2.0 * ",
                                 [SM_SRC_ABS] = "abs(", [SM_SRC_ABSNEG] = "(-abs(" };
    static const char *post[] = { [SM_SRC_NONE] = ")", [SM_SRC_NEG] = ")", [SM_SRC_BIAS] = ") - 0.5)",
                                  [SM_SRC_BIASNEG] = ") - 0.5))", [SM_SRC_SIGN] = " - 1.0)",
                                  [SM_SRC_SIGNNEG] = " + 1.0)", [SM_SRC_COMP] = ")", [SM_SRC_X2] = ")",
                                  [SM_SRC_X2NEG] = ")", [SM_SRC_ABS] = ")", [SM_SRC_ABSNEG] = "))" };
    if (r->mod > SM_SRC_ABSNEG || !pre[r->mod]) unsupported(t, "that source modifier");
    put(b, "%s", pre[r->mod]);
    reg_base(t, r, 0);
    uint8_t sw = r->mask_or_swizzle;
    if (n == 1) put(b, ".%c", XYZW[(sw >> 6) & 3]);
    else if (sw != SM_SWIZZLE_IDENTITY)
        put(b, ".%c%c%c%c", XYZW[sw & 3], XYZW[(sw >> 2) & 3], XYZW[(sw >> 4) & 3], XYZW[(sw >> 6) & 3]);
    put(b, "%s", post[r->mod]);
}

/* Source k of a matrix macro, row j: the register index plus j. */
static void src_row(struct tr *t, int k, int j, int three) {
    struct sm_ins tmp = *t->ins;
    tmp.src[k].index = (uint16_t)(tmp.src[k].index + j);
    const struct sm_ins *save = t->ins;
    t->ins = &tmp;
    src(t, k, 4);
    t->ins = save;
    if (three) put(t->b, ".xyz");
}

/* The result v (a float4 variable) to the destination, through its mask (and saturate / shift for ps). */
static void store_v(struct tr *t, const char *v, int addr_round) {
    const struct sm_ins *in = t->ins;
    const struct sm_reg *d = &in->dst;
    struct sb *b = t->b;
    if (d->shift) {
        static const char *sh[] = { "0.125", "0.25", "0.5", "", "2.0", "4.0", "8.0" };
        put(b, "  %s *= %s;\n", v, sh[d->shift + 3]);
    }
    if (d->mod & SM_DST_SAT) put(b, "  %s = saturate(%s);\n", v, v);
    unsigned m = d->mask_or_swizzle & 0xF;
    if (d->type == SM_R_ADDR && !t->s->is_ps) {
        for (int i = 0; i < 4; i++)
            if (m & (1u << i))
                put(b, "  s.a.%c = int(%s(%s.%c));\n", XYZW[i], addr_round ? "round" : "floor", v, XYZW[i]);
        return;
    }
    put(b, "  ");
    reg_base(t, d, 1);
    if (m != 0xF) {
        put(b, ".");
        for (int i = 0; i < 4; i++) if (m & (1u << i)) put(b, "%c", XYZW[i]);
        put(b, " = %s.", v);
        for (int i = 0; i < 4; i++) if (m & (1u << i)) put(b, "%c", XYZW[i]);
        put(b, ";\n");
    } else put(b, " = %s;\n", v);
}
static void store(struct tr *t, int addr_round) { store_v(t, "t", addr_round); }

static void bool_test(struct tr *t, int k) {
    const struct sm_reg *r = &t->ins->src[k];
    if (r->type != SM_R_CONSTBOOL) unsupported(t, "a condition that is not a boolean constant");
    put(t->b, "k.b[%u] %s 0u", r->index, r->mod == SM_SRC_NOT ? "==" : "!=");
}

/* One arithmetic instruction's result, as a float4 expression. */
static void arith_expr(struct tr *t) {
    const struct sm_ins *in = t->ins;
    struct sb *b = t->b;
    int v11 = t->s->major == 1 && !t->s->is_ps;
#define S(k) src(t, k, 4)
#define S1(k) src(t, k, 1)
    switch (in->op) {
    case SM_MOV: case SM_MOVA: S(0); break;
    case SM_ADD: S(0); put(b, " + "); S(1); break;
    case SM_SUB: S(0); put(b, " - "); S(1); break;
    case SM_MUL: S(0); put(b, " * "); S(1); break;
    case SM_MAD: S(0); put(b, " * "); S(1); put(b, " + "); S(2); break;
    case SM_DP3: put(b, "float4(dot("); S(0); put(b, ".xyz, "); S(1); put(b, ".xyz))"); break;
    case SM_DP4: put(b, "float4(dot("); S(0); put(b, ", "); S(1); put(b, "))"); break;
    case SM_MIN: put(b, "min("); S(0); put(b, ", "); S(1); put(b, ")"); break;
    case SM_MAX: put(b, "max("); S(0); put(b, ", "); S(1); put(b, ")"); break;
    case SM_SLT: put(b, "sm_slt("); S(0); put(b, ", "); S(1); put(b, ")"); break;
    case SM_SGE: put(b, "sm_sge("); S(0); put(b, ", "); S(1); put(b, ")"); break;
    case SM_RCP: put(b, "float4(sm_rcp("); S1(0); put(b, "))"); break;
    case SM_RSQ: put(b, "float4(sm_rsq("); S1(0); put(b, "))"); break;
    case SM_EXP: put(b, "float4(exp2("); S1(0); put(b, "))"); break;
    case SM_LOG: put(b, "float4(sm_log("); S1(0); put(b, "))"); break;
    case SM_EXPP:
        if (v11) { put(b, "sm_expp11("); S1(0); put(b, ")"); }
        else { put(b, "float4(exp2("); S1(0); put(b, "))"); }
        break;
    case SM_LOGP:
        if (v11) { put(b, "sm_logp11("); S1(0); put(b, ")"); }
        else { put(b, "float4(sm_log("); S1(0); put(b, "))"); }
        break;
    case SM_LIT: put(b, "sm_lit("); S(0); put(b, ")"); break;
    case SM_DST: put(b, "float4(1.0, "); S(0); put(b, ".y * "); S(1); put(b, ".y, "); S(0); put(b, ".z, ");
        S(1); put(b, ".w)"); break;
    case SM_LRP: put(b, "mix("); S(2); put(b, ", "); S(1); put(b, ", "); S(0); put(b, ")"); break;
    case SM_FRC: put(b, "fract("); S(0); put(b, ")"); break;
    case SM_M4x4: case SM_M4x3: case SM_M3x4: case SM_M3x3: case SM_M3x2: {
        int rows = in->op == SM_M4x4 || in->op == SM_M3x4 ? 4 : in->op == SM_M3x2 ? 2 : 3;
        int three = in->op != SM_M4x4 && in->op != SM_M4x3;
        put(b, "float4(");
        for (int j = 0; j < 4; j++) {
            if (j) put(b, ", ");
            if (j >= rows) { put(b, "0.0"); continue; }
            put(b, "dot(");
            S(0);
            if (three) put(b, ".xyz");
            put(b, ", ");
            src_row(t, 1, j, three);
            put(b, ")");
        }
        put(b, ")");
        break;
    }
    case SM_POW: put(b, "float4(sm_pow("); S1(0); put(b, ", "); S1(1); put(b, "))"); break;
    case SM_CRS: put(b, "float4(cross("); S(0); put(b, ".xyz, "); S(1); put(b, ".xyz), 0.0)"); break;
    case SM_SGN: put(b, "sign("); S(0); put(b, ")"); break;
    case SM_ABS: put(b, "abs("); S(0); put(b, ")"); break;
    case SM_NRM: put(b, "sm_nrm("); S(0); put(b, ")"); break;
    case SM_SINCOS: put(b, "float4(cos("); S1(0); put(b, "), sin("); S1(0); put(b, "), 0.0, 0.0)"); break;
    /* pixel shaders: cnd tests r0.a in 1.1-1.3 (its only source 0), each component in 1.4 */
    case SM_CND:
        if (t->s->major == 1 && t->s->minor < 4) {
            put(b, "("); S1(0); put(b, " > 0.5 ? "); S(1); put(b, " : "); S(2); put(b, ")");
        } else { put(b, "select("); S(2); put(b, ", "); S(1); put(b, ", "); S(0); put(b, " > 0.5)"); }
        break;
    case SM_CMP: put(b, "select("); S(2); put(b, ", "); S(1); put(b, ", "); S(0); put(b, " >= 0.0)"); break;
    case SM_DP2ADD: put(b, "float4(dot("); S(0); put(b, ".xy, "); S(1); put(b, ".xy) + "); S1(2); put(b, ")"); break;
    case SM_BEM: {                                 /* the bump matrix of the destination's stage */
        unsigned n = in->dst.index & 7;
        put(b, "float4("); S(0); put(b, ".x + u.bumpmat[%u].x * ", n); S(1); put(b, ".x + u.bumpmat[%u].z * ", n);
        S(1); put(b, ".y, "); S(0); put(b, ".y + u.bumpmat[%u].y * ", n); S(1); put(b, ".x + u.bumpmat[%u].w * ", n);
        S(1); put(b, ".y, 0.0, 0.0)");
        break;
    }
    default: unsupported(t, NULL);
    }
#undef S
#undef S1
}

/* One arithmetic instruction: "{ float4 t = <expr>; <store> }". */
static void arith(struct tr *t) {
    put(t->b, " {\n  float4 t = ");
    arith_expr(t);
    put(t->b, ";\n");
    store(t, t->ins->op == SM_MOVA);
    put(t->b, " }\n");
}

/* The body from instruction i up to the next label (or the end). Returns where it stopped. */
static uint32_t body(struct tr *t, uint32_t i, int *loops) {
    const struct sm_shader *s = t->s;
    struct sb *b = t->b;
    int stack[64], depth = 0;
    for (; i < s->nins; i++) {
        const struct sm_ins *in = &s->ins[i];
        t->ins = in;
        switch (in->op) {
        case SM_NOP: case SM_DCL: case SM_DEF: case SM_DEFI: case SM_DEFB: break;
        case SM_LABEL: return i;
        case SM_RET: put(b, "  return;\n"); break;
        case SM_IF: put(b, "  if ("); bool_test(t, 0); put(b, ") {\n"); depth++; break;
        case SM_ELSE: put(b, "  } else {\n"); break;
        case SM_ENDIF: put(b, "  }\n"); depth--; break;
        case SM_LOOP: {
            if (in->src[1].type != SM_R_CONSTINT) unsupported(t, "a loop not counted by an integer constant");
            if (depth >= 64) unsupported(t, "nesting this deep");
            int n = (*loops)++;
            stack[depth++] = n;
            put(b, "  { int sv%d = s.aL; int4 li%d = k.i[%u]; s.aL = li%d.y;\n"
                   "  for (int n%d = 0; n%d < clamp(li%d.x, 0, 255); n%d++, s.aL += li%d.z) {\n",
                n, n, in->src[1].index, n, n, n, n, n, n);
            break;
        }
        case SM_ENDLOOP:
            if (!depth) unsupported(t, "endloop without loop");
            depth--;
            put(b, "  }\n  s.aL = sv%d; }\n", stack[depth]);
            break;
        case SM_REP: {
            if (in->src[0].type != SM_R_CONSTINT) unsupported(t, "a rep not counted by an integer constant");
            int n = (*loops)++;
            put(b, "  for (int n%d = 0; n%d < clamp(k.i[%u].x, 0, 255); n%d++) {\n", n, n, in->src[0].index, n);
            depth++;
            break;
        }
        case SM_ENDREP: put(b, "  }\n"); depth--; break;
        case SM_CALL: put(b, "  sm_l%u(s, c, k);\n", in->src[0].index); break;
        case SM_CALLNZ:
            put(b, "  if (");
            bool_test(t, 1);
            put(b, ") sm_l%u(s, c, k);\n", in->src[0].index);
            break;
        default: arith(t);
        }
    }
    return i;
}

char *sm_msl_vertex(const struct sm_shader *s, const struct sm_vs_variant *v) {
    struct sb b = { 0 };
    struct tr t = { &b, s, NULL, "" };
    sm_version_name(s->version, t.ver, sizeof t.ver);
    if (s->is_ps) d3d_fail("%s given to the vertex shader translator", t.ver);
    char *vo = ff_msl_vout(8, v->flat, v->points, v->nclip);
    put(&b, "%s%s", vo, SM_HELPERS);
    free(vo);
    /* the inputs: each declared register that has an element */
    uint32_t declared = 0;
    for (uint32_t i = 0; i < s->nins; i++)
        if (s->ins[i].op == SM_DCL && s->ins[i].dst.type == SM_R_INPUT && s->ins[i].dst.index < 16)
            declared |= 1u << s->ins[i].dst.index;
    int any = 0;
    for (int r = 0; r < 16; r++)
        if ((declared >> r & 1) && v->input[r] != SM_IN_NONE) {
            static const char *ty[] = { "", "float4", "uint4", "int4" };
            if (!any) put(&b, "struct VIn {\n");
            put(&b, "  %s v%d [[attribute(%d)]];\n", ty[v->input[r] & 3], r, r);
            any = 1;
        }
    if (any) put(&b, "};\n");
    put(&b, "struct S { float4 r[12]; float4 v[16]; int4 a; int aL; float4 opos, ofog, opts; float4 od[2]; "
            "float4 ot[8]; };\n");
    /* the labels: prototypes, then main and each subroutine */
    uint32_t labels = 0;
    for (uint32_t i = 0; i < s->nins; i++)
        if (s->ins[i].op == SM_LABEL) {
            t.ins = &s->ins[i];
            if (s->ins[i].src[0].index >= 32) unsupported(&t, "a label above l31");
            labels |= 1u << s->ins[i].src[0].index;
        }
    for (int l = 0; l < 32; l++)
        if (labels >> l & 1) put(&b, "void sm_l%d(thread S &s, constant float4 *c, constant SMK &k);\n", l);
    int loops = 0;
    put(&b, "void sm_main(thread S &s, constant float4 *c, constant SMK &k) {\n");
    uint32_t i = body(&t, 0, &loops);
    put(&b, "}\n");
    while (i < s->nins) {                       /* at a label */
        put(&b, "void sm_l%u(thread S &s, constant float4 *c, constant SMK &k) {\n", s->ins[i].src[0].index);
        i = body(&t, i + 1, &loops);
        put(&b, "}\n");
    }
    put(&b, "vertex VOut vsh(%sconstant VU &u [[buffer(0)]], constant float4 *c [[buffer(1)]], "
            "constant SMK &k [[buffer(2)]]) {\n  S s = {};\n", any ? "VIn in [[stage_in]], " : "");
    for (int r = 0; r < 16; r++) {
        if (!(declared >> r & 1)) continue;
        if (v->input[r] == SM_IN_NONE) put(&b, "  s.v[%d] = float4(0.0, 0.0, 0.0, 1.0);\n", r);
        else put(&b, "  s.v[%d] = float4(in.v%d);\n", r, r);
    }
    put(&b, "  s.od[0] = s.od[1] = float4(1.0);\n  s.ofog = float4(1.0);\n  s.opts = float4(u.power.z);\n"
            "  sm_main(s, c, k);\n  VOut o;\n  o.pos = s.opos;\n");
    for (int p = 0; p < v->nclip; p++) put(&b, "  o.clip[%d] = dot(s.opos, u.clip[%d]);\n", p, p);
    /* table fog's depth as the fixed pipeline has it; then D3D9's pixel centres (half a pixel) */
    put(&b, "  o.fogz = u.power.y != 0.0 ? o.pos.w : o.pos.z / o.pos.w;\n"
            "  o.pos.xy += float2(u.vpz.z, -u.vpz.w) * o.pos.w;\n"
            "  o.c0 = saturate(s.od[0]);\n  o.c1 = saturate(s.od[1]);\n");
    for (int n = 0; n < 8; n++) put(&b, "  o.tc%d = s.ot[%d];\n", n, n);
    put(&b, "  o.fog = s.ofog.x;\n");
    if (v->points) put(&b, "  o.psize = s.opts.x;\n");
    put(&b, "  return o;\n}\n");
    return b.s;
}

/* ---- Task 4: the pixel side ---- */

uint32_t sm_ps_samplers(const struct sm_shader *s, uint32_t *projectable) {
    uint32_t m = 0, pj = 0;
    for (uint32_t i = 0; i < s->nins; i++) {
        const struct sm_ins *in = &s->ins[i];
        uint32_t bit = in->ndst && in->dst.index < 16 ? 1u << in->dst.index : 0;
        if (s->major >= 2) {
            if (in->op == SM_DCL && in->dst.type == SM_R_SAMPLER) m |= bit;
        } else if (s->minor == 4) {
            if (in->op == SM_TEX) m |= bit;
        } else switch (in->op) {
            case SM_TEX: case SM_TEXBEM: case SM_TEXBEML: pj |= bit; m |= bit; break;
            case SM_TEXREG2AR: case SM_TEXREG2GB: case SM_TEXREG2RGB: case SM_TEXM3x2TEX: case SM_TEXM3x3TEX:
            case SM_TEXM3x3SPEC: case SM_TEXM3x3VSPEC: case SM_TEXDP3TEX: m |= bit; break;
            default: break;
            }
    }
    if (projectable) *projectable = pj;
    return m;
}

void sm_ps_variant_default(const struct sm_shader *s, struct sm_ps_variant *v) {
    memset(v, 0, sizeof *v);
    uint32_t m = sm_ps_samplers(s, NULL);
    for (int i = 0; i < 16; i++) if (m >> i & 1) v->textype[i] = SM_TEX_2D;
    for (uint32_t i = 0; i < s->nins; i++) {
        const struct sm_ins *in = &s->ins[i];
        if (in->op == SM_DCL && in->dst.type == SM_R_SAMPLER && in->dst.index < 16)
            v->textype[in->dst.index] = (uint8_t)(in->sampler_type - SM_STT_2D + SM_TEX_2D);
    }
}

/* Sampler i read at the float4 expression co (2D: .xy; cube and volume: .xyz, as the bound texture is), with
 * the sampler's LOD bias plus bias (an expression, or NULL). */
static void sample(struct tr *t, unsigned i, const char *co, const char *bias) {
    int ty = i < 16 ? t->pv->textype[i] : 0;
    if (!ty) { put(t->b, "float4(0.0, 0.0, 0.0, 1.0)"); return; }
    put(t->b, "tex%u.sample(smp%u, (%s).%s, bias(u.lodbias[%u].x%s%s))", i, i, co, ty == SM_TEX_2D ? "xy" : "xyz", i,
        bias ? " + " : "", bias ? bias : "");
}

/* ps 1.1-1.3: stage m's texture coordinates, divided as D3DTTFF_PROJECTED says. */
static void stage_coords(struct tr *t, unsigned m) {
    put(t->b, "  float4 co = in.tc%u;\n", m & 7);
    unsigned p = t->pv->proj[m & 7];
    if (p >= 1 && p <= 4) put(t->b, "  co /= co.%c;\n", XYZW[p - 1]);
}

/* Source k without its _dz/_dw modifier, then that divide applied (ps_1_4 texld and texcrd): into co. */
static void coords_14(struct tr *t, int k) {
    struct sm_ins tmp = *t->ins;
    uint8_t mod = tmp.src[k].mod;
    if (mod == SM_SRC_DZ || mod == SM_SRC_DW) tmp.src[k].mod = SM_SRC_NONE;
    const struct sm_ins *save = t->ins;
    t->ins = &tmp;
    put(t->b, "  float4 co = ");
    src(t, k, 4);
    put(t->b, ";\n");
    t->ins = save;
    if (mod == SM_SRC_DZ) put(t->b, "  co.xy /= co.z;\n");
    else if (mod == SM_SRC_DW) put(t->b, "  co.xy /= co.w;\n");
}

/* The texture instructions. Returns 0 if in is not one. */
static int ps_texop(struct tr *t) {
    const struct sm_ins *in = t->ins;
    const struct sm_shader *s = t->s;
    struct sb *b = t->b;
    unsigned m = in->dst.index;
    int v13 = s->major == 1 && s->minor < 4, v14 = s->major == 1 && s->minor == 4;
    switch (in->op) {
    case SM_TEX:
        put(b, " {\n");
        if (s->major >= 2) {
            put(b, "  float4 co = ");
            src(t, 0, 4);
            put(b, ";\n");
            if (in->control == SM_TEXLD_PROJECT) put(b, "  co /= co.w;\n");
            put(b, "  float4 t = ");
            sample(t, in->src[1].index, "co", in->control == SM_TEXLD_BIAS ? "co.w" : NULL);
        } else if (v14) {
            coords_14(t, 0);
            put(b, "  float4 t = ");
            sample(t, m, "co", NULL);
        } else {
            stage_coords(t, m);
            put(b, "  float4 t = ");
            sample(t, m, "co", NULL);
        }
        put(b, ";\n");
        store(t, 0);
        put(b, " }\n");
        return 1;
    case SM_TEXCOORD:
        put(b, " {\n");
        if (v14) { coords_14(t, 0); put(b, "  float4 t = co;\n"); }
        else put(b, "  float4 t = float4(saturate(in.tc%u.xyz), 1.0);\n", m & 7);
        store(t, 0);
        put(b, " }\n");
        return 1;
    case SM_TEXKILL:
        if (v13 || (v14 && in->dst.type == SM_R_TEXTURE)) {
            put(b, "  if (any(in.tc%u.xyz < 0.0)) discard_fragment();\n", m & 7);
        } else if (v14) {
            put(b, "  if (any(");
            reg_base(t, &in->dst, 0);
            put(b, ".xyz < 0.0)) discard_fragment();\n");
        } else {
            unsigned k = in->dst.mask_or_swizzle & 0xF;
            put(b, "  if (");
            const char *sep = "";
            for (int c = 0; c < 4; c++)
                if (k >> c & 1) { put(b, "%s", sep); reg_base(t, &in->dst, 0); put(b, ".%c < 0.0", XYZW[c]); sep = " || "; }
            put(b, ") discard_fragment();\n");
        }
        return 1;
    case SM_TEXDEPTH:                              /* r5.r / r5.g to the depth */
        put(b, "  s.od = float4(s.r[5].y == 0.0 ? 1.0 : s.r[5].x / s.r[5].y);\n");
        return 1;
    default: break;
    }
    if (!v13) return 0;
    /* ps 1.1-1.3 texture addressing: t(m) from stage m and the register t(n) (source 0) */
#define TN() src(t, 0, 4)
    switch (in->op) {
    case SM_TEXBEM: case SM_TEXBEML:
        put(b, " {\n");
        stage_coords(t, m);
        put(b, "  float4 d = ");
        TN();
        put(b, ";\n  co.xy += float2(u.bumpmat[%u].x * d.x + u.bumpmat[%u].z * d.y, "
               "u.bumpmat[%u].y * d.x + u.bumpmat[%u].w * d.y);\n  float4 t = ", m & 7, m & 7, m & 7, m & 7);
        sample(t, m, "co", NULL);
        put(b, ";\n");
        if (in->op == SM_TEXBEML)
            put(b, "  t.rgb *= saturate(d.z * u.bumpl[%u].x + u.bumpl[%u].y);\n", m & 7, m & 7);
        store(t, 0);
        put(b, " }\n");
        break;
    case SM_TEXREG2AR: case SM_TEXREG2GB: case SM_TEXREG2RGB:
        put(b, " {\n  float4 d = ");
        TN();
        put(b, ";\n  float4 co = %s;\n  float4 t = ", in->op == SM_TEXREG2AR ? "float4(d.w, d.x, 0.0, 0.0)"
                                                  : in->op == SM_TEXREG2GB ? "float4(d.y, d.z, 0.0, 0.0)"
                                                                           : "float4(d.xyz, 0.0)");
        sample(t, m, "co", NULL);
        put(b, ";\n");
        store(t, 0);
        put(b, " }\n");
        break;
    case SM_TEXM3x2PAD: case SM_TEXM3x3PAD:
        if (t->npad > 1) unsupported(t, "a third pad row");
        put(b, "  s.m3.%c = dot(in.tc%u.xyz, (", XYZW[t->npad], m & 7);
        TN();
        put(b, ").xyz);\n");
        t->npad++;
        return 1;                                  /* the next row continues the matrix */
    case SM_TEXM3x2TEX: case SM_TEXM3x2DEPTH:
        if (t->npad != 1) unsupported(t, "without one texm3x2pad before it");
        put(b, " {\n  s.m3.y = dot(in.tc%u.xyz, (", m & 7);
        TN();
        put(b, ").xyz);\n");
        if (in->op == SM_TEXM3x2DEPTH) put(b, "  s.od = float4(s.m3.y == 0.0 ? 1.0 : s.m3.x / s.m3.y);\n }\n");
        else {
            put(b, "  float4 co = float4(s.m3.xy, 0.0, 0.0);\n  float4 t = ");
            sample(t, m, "co", NULL);
            put(b, ";\n");
            store(t, 0);
            put(b, " }\n");
        }
        break;
    case SM_TEXM3x3TEX: case SM_TEXM3x3: case SM_TEXM3x3SPEC: case SM_TEXM3x3VSPEC:
        if (t->npad != 2) unsupported(t, "without two texm3x3pad before it");
        put(b, " {\n  s.m3.z = dot(in.tc%u.xyz, (", m & 7);
        TN();
        put(b, ").xyz);\n  float3 N = s.m3;\n");
        if (in->op == SM_TEXM3x3) put(b, "  float4 t = float4(N, 1.0);\n");
        else {
            if (in->op == SM_TEXM3x3SPEC) { put(b, "  float3 E = ("); src(t, 1, 4); put(b, ").xyz;\n"); }
            else if (in->op == SM_TEXM3x3VSPEC)
                put(b, "  float3 E = float3(in.tc%u.w, in.tc%u.w, in.tc%u.w);\n", (m - 2) & 7, (m - 1) & 7, m & 7);
            if (in->op == SM_TEXM3x3TEX) put(b, "  float4 co = float4(N, 0.0);\n");
            else put(b, "  float4 co = float4(2.0 * N * dot(N, E) / dot(N, N) - E, 0.0);\n");
            put(b, "  float4 t = ");
            sample(t, m, "co", NULL);
            put(b, ";\n");
        }
        store(t, 0);
        put(b, " }\n");
        break;
    case SM_TEXDP3TEX: case SM_TEXDP3:
        put(b, " {\n  float dp = dot(in.tc%u.xyz, (", m & 7);
        TN();
        put(b, ").xyz);\n");
        if (in->op == SM_TEXDP3) put(b, "  float4 t = float4(dp);\n");
        else { put(b, "  float4 co = float4(dp, 0.0, 0.0, 0.0);\n  float4 t = "); sample(t, m, "co", NULL); put(b, ";\n"); }
        store(t, 0);
        put(b, " }\n");
        break;
    default: return 0;
    }
#undef TN
    t->npad = 0;
    return 1;
}

char *sm_msl_fragment(const struct sm_shader *s, const struct sm_ps_variant *v) {
    struct sb b = { 0 };
    struct tr t = { &b, s, NULL, "", v, 0 };
    sm_version_name(s->version, t.ver, sizeof t.ver);
    if (!s->is_ps) d3d_fail("%s given to the pixel shader translator", t.ver);
    char *vo = ff_msl_vout(8, v->flat, 0, 0);
    put(&b, "%s%s", vo, SM_HELPERS);
    free(vo);
    /* the outputs: ps 1.x's r0 is colour 0; 2.0 writes oC# and oDepth */
    uint32_t ocs = s->major == 1 ? 1 : 0;
    int depth = 0;
    for (uint32_t i = 0; i < s->nins; i++) {
        const struct sm_ins *in = &s->ins[i];
        if (in->op == SM_TEXDEPTH || in->op == SM_TEXM3x2DEPTH) depth = 1;
        if (!in->ndst || in->op == SM_DCL || in->op == SM_DEF) continue;
        if (in->dst.type == SM_R_COLOROUT && in->dst.index < 4) ocs |= 1u << in->dst.index;
        if (in->dst.type == SM_R_DEPTHOUT) depth = 1;
    }
    ocs |= 1;                                      /* colour 0 always: fog and the alpha test read it */
    put(&b, "struct FOut {\n");
    for (int i = 0; i < 4; i++) if (ocs >> i & 1) put(&b, "  float4 c%d [[color(%d)]];\n", i, i);
    if (depth) put(&b, "  float depth [[depth(any)]];\n");
    put(&b, "};\n");
    put(&b, "struct PS { float4 r[12]; float4 t[8]; float4 v[2]; float4 oc[4]; float4 od; float3 m3; };\n");
    put(&b, "fragment FOut psh(VOut in [[stage_in]], constant PU &u [[buffer(0)]], constant float4 *c [[buffer(1)]]");
    static const char *const types[] = { NULL, "texture2d<float>", "texturecube<float>", "texture3d<float>" };
    for (int i = 0; i < 16; i++)
        if (v->textype[i] >= 1 && v->textype[i] <= 3)
            put(&b, ", %s tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", types[v->textype[i]], i, i, i, i);
    put(&b, ") {\n  PS s = {};\n  s.v[0] = in.c0;\n  s.v[1] = in.c1;\n");
    for (int i = 0; i < 8; i++) put(&b, "  s.t[%d] = in.tc%d;\n", i, i);
    for (uint32_t i = 0; i < s->nins; i++) {
        const struct sm_ins *in = &s->ins[i];
        t.ins = in;
        switch (in->op) {
        case SM_NOP: case SM_DCL: case SM_DEF: case SM_PHASE: continue;
        case SM_TEXM3x2PAD: case SM_TEXM3x3PAD: case SM_TEXM3x2TEX: case SM_TEXM3x2DEPTH: case SM_TEXM3x3TEX:
        case SM_TEXM3x3: case SM_TEXM3x3SPEC: case SM_TEXM3x3VSPEC: break;
        default: if (t.npad) unsupported(&t, "after an unfinished texm3x2pad/texm3x3pad");
        }
        if (ps_texop(&t)) continue;
        if (i + 1 < s->nins && s->ins[i + 1].coissue) {
            /* co-issued: both read their sources before either writes */
            const struct sm_ins *in2 = &s->ins[i + 1];
            put(&b, " {\n  float4 t = ");
            arith_expr(&t);
            put(&b, ";\n  float4 t2 = ");
            t.ins = in2;
            arith_expr(&t);
            put(&b, ";\n");
            t.ins = in;
            store_v(&t, "t", 0);
            t.ins = in2;
            store_v(&t, "t2", 0);
            put(&b, " }\n");
            i++;
            continue;
        }
        arith(&t);
    }
    put(&b, "  FOut o;\n  float4 col = %s;\n", s->major == 1 ? "saturate(s.r[0])" : "s.oc[0]");
    char *post = ff_msl_post(v->fog, v->alpha_func, v->srgb, "col");
    put(&b, "%s  o.c0 = col;\n", post);
    free(post);
    for (int i = 1; i < 4; i++)
        if (ocs >> i & 1) {
            char var[16];
            snprintf(var, sizeof var, "s.oc[%d]", i);
            char *enc = ff_msl_post(0, 0, v->srgb, var);
            put(&b, "%s  o.c%d = s.oc[%d];\n", enc, i, i);
            free(enc);
        }
    if (depth) put(&b, "  o.depth = saturate(s.od.x);\n");
    put(&b, "  return o;\n}\n");
    return b.s;
}
