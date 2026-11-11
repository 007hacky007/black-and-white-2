/*
 * sm_msl.c - D3D9 shader bytecode to Metal Shading Language (plan 5).
 * Task 3: the vertex side. Each vertex shader becomes one `vertex VOut vsh`
 * that writes the VOut every fragment function reads (ff_msl.c's emitter).
 * The register file is one struct; each label is a function taking it by
 * reference. Semantics follow the D3D9 shader reference as pinned in the
 * plan's Global Constraints.
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

/* What the translation is working on, for messages. */
struct tr { struct sb *b; const struct sm_shader *s; const struct sm_ins *ins; char ver[16]; };

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

/* A register as an lvalue or base expression. */
static void reg_base(struct tr *t, const struct sm_reg *r, int dst) {
    struct sb *b = t->b;
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
    char rb[32], msg[64];
    snprintf(msg, sizeof msg, "register %s as a %s", sm_reg_name(t->s->version, r, rb, sizeof rb),
             dst ? "destination" : "source");
    unsupported(t, msg);
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

/* The result t (a float4) to the destination, through its mask (and saturate / shift for ps). */
static void store(struct tr *t, int addr_round) {
    const struct sm_ins *in = t->ins;
    const struct sm_reg *d = &in->dst;
    struct sb *b = t->b;
    if (d->shift) {
        static const char *sh[] = { "0.125", "0.25", "0.5", "", "2.0", "4.0", "8.0" };
        put(b, "  t *= %s;\n", sh[d->shift + 3]);
    }
    if (d->mod & SM_DST_SAT) put(b, "  t = saturate(t);\n");
    unsigned m = d->mask_or_swizzle & 0xF;
    if (d->type == SM_R_ADDR) {
        for (int i = 0; i < 4; i++)
            if (m & (1u << i))
                put(b, "  s.a.%c = int(%s(t.%c));\n", XYZW[i], addr_round ? "round" : "floor", XYZW[i]);
        return;
    }
    put(b, "  ");
    reg_base(t, d, 1);
    if (m != 0xF) {
        put(b, ".");
        for (int i = 0; i < 4; i++) if (m & (1u << i)) put(b, "%c", XYZW[i]);
        put(b, " = t.");
        for (int i = 0; i < 4; i++) if (m & (1u << i)) put(b, "%c", XYZW[i]);
        put(b, ";\n");
    } else put(b, " = t;\n");
}

static void bool_test(struct tr *t, int k) {
    const struct sm_reg *r = &t->ins->src[k];
    if (r->type != SM_R_CONSTBOOL) unsupported(t, "a condition that is not a boolean constant");
    put(t->b, "k.b[%u] %s 0u", r->index, r->mod == SM_SRC_NOT ? "==" : "!=");
}

/* One arithmetic instruction: "{ float4 t = <expr>; <store> }". */
static void arith(struct tr *t) {
    const struct sm_ins *in = t->ins;
    struct sb *b = t->b;
    int v11 = t->s->major == 1;
    put(b, " {\n  float4 t = ");
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
    default: unsupported(t, NULL);
    }
#undef S
#undef S1
    put(b, ";\n");
    store(t, in->op == SM_MOVA);
    put(b, " }\n");
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
