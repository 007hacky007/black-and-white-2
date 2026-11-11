/*
 * ff_msl.c - Metal shaders for the fixed-function pipeline, generated from
 * its state key (plan 4 Task 6). Formulas are the D3D9 documentation's:
 * transform, lighting, texture coordinate generation and transforms, vertex
 * and table fog, the texture stage combiner, specular add and alpha test.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"

struct sb { char *s; size_t n, cap; };
static void put(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct sb *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        b->cap = b->cap ? b->cap * 2 + (size_t)k : 8192;
        b->s = realloc(b->s, b->cap);
        if (!b->s) abort();
    }
}

static const char *HEADER =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Light { float4 diffuse, specular, ambient, position, direction, p0, p1; };\n"
    "struct VU { float4x4 wv, proj, wvp, normal_m, view, world; float4x4 tex[8];\n"
    "  float4 vp, vpz, md, ma, ms, me, ambient, fog; float4 clip[6]; float4 power; Light light[8]; };\n"
    "struct PU { float4 tfactor; float4 cst[8]; float4 bumpmat[8]; float4 bumpl[8]; float4 lodbias[8];\n"
    "  float4 fogcolor; float4 fog; };\n";

/* D3DTOP */
enum { DISABLE = 1, SELECTARG1, SELECTARG2, MODULATE, MODULATE2X, MODULATE4X, ADD, ADDSIGNED, ADDSIGNED2X, SUBTRACT,
       ADDSMOOTH, BLENDDIFFUSEALPHA, BLENDTEXTUREALPHA, BLENDFACTORALPHA, BLENDTEXTUREALPHAPM, BLENDCURRENTALPHA,
       PREMODULATE, MODULATEALPHA_ADDCOLOR, MODULATECOLOR_ADDALPHA, MODULATEINVALPHA_ADDCOLOR,
       MODULATEINVCOLOR_ADDALPHA, BUMPENVMAP, BUMPENVMAPLUMINANCE, DOTPRODUCT3, MULTIPLYADD, LERP };

/* The one VOut emitter: the fixed-function stages and the translated shaders agree through it. */
static void vout(struct sb *b, unsigned ntc, int flat, int points, unsigned nclip) {
    put(b, "struct VOut {\n  float4 pos [[position]];\n");
    put(b, "  float4 c0%s;\n  float4 c1%s;\n", flat ? " [[flat]]" : "", flat ? " [[flat]]" : "");
    for (unsigned i = 0; i < ntc; i++) put(b, "  float4 tc%u;\n", i);
    put(b, "  float fog;\n  float fogz;\n");
    if (points) put(b, "  float psize [[point_size]];\n");
    if (nclip) put(b, "  float clip [[clip_distance]] [%u];\n", nclip);
    put(b, "};\n");
}

char *ff_msl_vout(unsigned ntc, int flat, int points, unsigned nclip) {
    struct sb b = { 0 };
    put(&b, "%s", HEADER);
    vout(&b, ntc, flat, points, nclip);
    return b.s;
}

static void vertex(struct sb *b, const struct ff_vkey *k) {
    put(b, "struct VIn {\n  float4 pos [[attribute(%d)]];\n", VA_POSITION);
    if (k->has_normal) put(b, "  float4 normal [[attribute(%d)]];\n", VA_NORMAL);
    if (k->has_diffuse) put(b, "  float4 c0 [[attribute(%d)]];\n", VA_COLOR0);
    if (k->has_specular) put(b, "  float4 c1 [[attribute(%d)]];\n", VA_COLOR1);
    for (int i = 0; i < 8; i++) if (k->tex_comps[i]) put(b, "  float4 t%d [[attribute(%d)]];\n", i, VA_TEX0 + i);
    put(b, "};\n");
    vout(b, k->nstages, k->flat, k->points, k->nclip);
    put(b, "vertex VOut ffv(VIn in [[stage_in]], constant VU &u [[buffer(0)]]) {\n  VOut o;\n");
    put(b, "  float4 vc0 = %s, vc1 = %s;\n", k->has_diffuse ? "in.c0" : "float4(1.0)",
        k->has_specular ? "in.c1" : "float4(0.0)");
    if (k->pretransformed) {
        put(b, "  float w = in.pos.w != 0.0 ? 1.0 / in.pos.w : 1.0;\n"
               "  float2 ndc = float2((in.pos.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (in.pos.y - u.vp.y) / u.vp.w * 2.0);\n"
               "  float zr = u.vpz.y - u.vpz.x;\n"
               "  float z = zr != 0.0 ? (in.pos.z - u.vpz.x) / zr : in.pos.z;\n"
               "  o.pos = float4(ndc * w, z * w, w);\n"
               "  float4 vpos = float4(0.0, 0.0, in.pos.z, 1.0);\n"
               "  float3 N = float3(0.0, 0.0, 1.0);\n");
    } else {
        put(b, "  float4 p = float4(in.pos.xyz, 1.0);\n"
               "  float4 vpos = u.wv * p;\n"
               "  o.pos = u.proj * vpos;\n");
        if (k->has_normal)
            put(b, "  float3 N = (u.normal_m * float4(in.normal.xyz, 0.0)).xyz;\n%s",
                k->normalize ? "  N = normalize(N);\n" : "");
        else put(b, "  float3 N = float3(0.0);\n");
    }
    /* table fog's depth: eye-space w under a perspective projection (w-fog), else z */
    put(b, "  o.fogz = %s;\n", k->wfog ? "o.pos.w" : "o.pos.z / o.pos.w");
    /* D3D9 puts pixel centres on integer coordinates: half a pixel to the right and down of Metal's */
    put(b, "  o.pos.xy += float2(u.vpz.z, -u.vpz.w) * o.pos.w;\n");
    if (k->lighting && !k->pretransformed) {
        static const char *src[3] = { NULL, "vc0", "vc1" };
        const char *mat[4] = { "u.md", "u.ms", "u.ma", "u.me" };
        put(b, "  float4 Md = %s, Ms = %s, Ma = %s, Me = %s;\n",
            k->mat_src[0] ? src[k->mat_src[0]] : mat[0], k->mat_src[1] ? src[k->mat_src[1]] : mat[1],
            k->mat_src[2] ? src[k->mat_src[2]] : mat[2], k->mat_src[3] ? src[k->mat_src[3]] : mat[3]);
        put(b, "  float3 amb = u.ambient.rgb, dif = float3(0.0), spe = float3(0.0), P = vpos.xyz;\n");
        for (int j = 0; j < k->nlights; j++) {
            put(b, "  {\n    float3 L; float att = 1.0;\n");
            if (k->light_type[j] == 3) put(b, "    L = -u.light[%d].direction.xyz;\n", j);
            else {
                put(b, "    float3 d = u.light[%d].position.xyz - P; float dist = length(d);\n"
                       "    L = dist > 0.0 ? d / dist : float3(0.0, 0.0, 1.0);\n"
                       "    float4 q = u.light[%d].p0;\n"
                       "    att = dist > q.x ? 0.0 : 1.0 / max(q.z + q.w * dist + u.light[%d].p1.x * dist * dist, 1e-20);\n",
                    j, j, j);
                if (k->light_type[j] == 2)
                    put(b, "    float rho = dot(-L, u.light[%d].direction.xyz);\n"
                           "    float ct = u.light[%d].p1.y, cp = u.light[%d].p1.z;\n"
                           "    att *= rho > ct ? 1.0 : rho <= cp ? 0.0 : pow((rho - cp) / max(ct - cp, 1e-20), q.y);\n",
                        j, j, j);
            }
            put(b, "    amb += att * u.light[%d].ambient.rgb;\n"
                   "    float ndl = max(dot(N, L), 0.0);\n"
                   "    dif += att * u.light[%d].diffuse.rgb * ndl;\n", j, j);
            if (k->specular)
                put(b, "    float3 H = normalize(L + %s);\n"
                       "    spe += ndl > 0.0 ? att * u.light[%d].specular.rgb * pow(max(dot(N, H), 0.0), u.power.x) : float3(0.0);\n",
                    k->localviewer ? "normalize(-P)" : "float3(0.0, 0.0, -1.0)", j);
            put(b, "  }\n");
        }
        put(b, "  o.c0 = float4(saturate(Me.rgb + Ma.rgb * amb + Md.rgb * dif), Md.a);\n"
               "  o.c1 = float4(saturate(Ms.rgb * spe), Ms.a);\n");
    } else put(b, "  o.c0 = vc0;\n  o.c1 = vc1;\n");
    /* vertex fog: 1 EXP, 2 EXP2, 3 LINEAR (D3DFOGMODE) */
    if (k->fog_vertex && !k->pretransformed) {
        put(b, "  float fd = %s;\n", k->range_fog ? "length(vpos.xyz)" : "abs(vpos.z)");
        if (k->fog_vertex == 1) put(b, "  o.fog = saturate(exp(-fd * u.fog.z));\n");
        else if (k->fog_vertex == 2) put(b, "  o.fog = saturate(exp(-(fd * u.fog.z) * (fd * u.fog.z)));\n");
        else put(b, "  o.fog = saturate((u.fog.y - fd) / (u.fog.y - u.fog.x));\n");
    } else if (k->fog_from_specular) put(b, "  o.fog = o.c1.a;\n");
    else put(b, "  o.fog = 1.0;\n");
    /* texture coordinates per stage */
    for (int i = 0; i < k->nstages; i++) {
        int set = k->tci_set[i], mode = k->tci_mode[i], n = k->tex_comps[set];
        if (mode == 0 || k->pretransformed) {
            if (!n) put(b, "  float4 s%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
            else if (n == 1) put(b, "  float4 s%d = float4(in.t%d.x, 1.0, 0.0, 0.0);\n", i, set);
            else if (n == 2) put(b, "  float4 s%d = float4(in.t%d.xy, 1.0, 0.0);\n", i, set);
            else if (n == 3) put(b, "  float4 s%d = float4(in.t%d.xyz, 1.0);\n", i, set);
            else put(b, "  float4 s%d = in.t%d;\n", i, set);
        } else if (mode == 1) put(b, "  float4 s%d = float4(N, 1.0);\n", i);
        else if (mode == 2) put(b, "  float4 s%d = float4(vpos.xyz, 1.0);\n", i);
        else if (mode == 3) put(b, "  float4 s%d = float4(reflect(normalize(vpos.xyz), N), 1.0);\n", i);
        else {
            put(b, "  float3 r%d = reflect(normalize(vpos.xyz), N);\n"
                   "  float m%d = 2.0 * sqrt(r%d.x * r%d.x + r%d.y * r%d.y + (r%d.z - 1.0) * (r%d.z - 1.0));\n"
                   "  float4 s%d = float4(r%d.x / m%d + 0.5, -r%d.y / m%d + 0.5, 0.0, 1.0);\n",
                i, i, i, i, i, i, i, i, i, i, i, i, i);
        }
        if (k->ttf_count[i]) put(b, "  s%d = u.tex[%d] * s%d;\n", i, i, i);
        put(b, "  o.tc%d = s%d;\n", i, i);
    }
    if (k->points) put(b, "  o.psize = 1.0;\n");
    if (k->nclip) {
        put(b, "  float4 wp = u.world * float4(in.pos.xyz, 1.0);\n");
        for (int c = 0; c < k->nclip; c++) put(b, "  o.clip[%d] = dot(wp, u.clip[%d]);\n", c, c);
    }
    put(b, "  return o;\n}\n");
}

/* A D3DTA argument as an MSL float4 expression for stage i. */
static void arg(struct sb *b, uint8_t a, int i) {
    static const char *sel[] = { "diffuse", "current", "t%d", "u.tfactor", "specular", "temp", "u.cst[%d]" };
    int s = a & 0xF;
    char base[48];
    if (s > 6) s = 1;
    snprintf(base, sizeof base, sel[s], i);
    char x[64];
    if (a & 0x10) snprintf(x, sizeof x, "(1.0 - %s)", base);
    else snprintf(x, sizeof x, "%s", base);
    if (a & 0x20) put(b, "%s.aaaa", x);
    else put(b, "%s", x);
}

/* The result of op on arguments, as float4 (the caller keeps .rgb or .a). */
static void op_expr(struct sb *b, int op, const uint8_t args[3], int i) {
#define A1 arg(b, args[1], i)
#define A2 arg(b, args[2], i)
#define A0 arg(b, args[0], i)
    switch (op) {
    case SELECTARG1: A1; break;
    case SELECTARG2: A2; break;
    case MODULATE: put(b, "("); A1; put(b, " * "); A2; put(b, ")"); break;
    case MODULATE2X: put(b, "(2.0 * "); A1; put(b, " * "); A2; put(b, ")"); break;
    case MODULATE4X: put(b, "(4.0 * "); A1; put(b, " * "); A2; put(b, ")"); break;
    case ADD: put(b, "("); A1; put(b, " + "); A2; put(b, ")"); break;
    case ADDSIGNED: put(b, "("); A1; put(b, " + "); A2; put(b, " - 0.5)"); break;
    case ADDSIGNED2X: put(b, "(2.0 * ("); A1; put(b, " + "); A2; put(b, " - 0.5))"); break;
    case SUBTRACT: put(b, "("); A1; put(b, " - "); A2; put(b, ")"); break;
    case ADDSMOOTH: put(b, "("); A1; put(b, " + "); A2; put(b, " * (1.0 - "); A1; put(b, "))"); break;
    case BLENDDIFFUSEALPHA: put(b, "mix("); A2; put(b, ", "); A1; put(b, ", diffuse.a)"); break;
    case BLENDTEXTUREALPHA: put(b, "mix("); A2; put(b, ", "); A1; put(b, ", t%d.a)", i); break;
    case BLENDFACTORALPHA: put(b, "mix("); A2; put(b, ", "); A1; put(b, ", u.tfactor.a)"); break;
    case BLENDTEXTUREALPHAPM: put(b, "("); A1; put(b, " + "); A2; put(b, " * (1.0 - t%d.a))", i); break;
    case BLENDCURRENTALPHA: put(b, "mix("); A2; put(b, ", "); A1; put(b, ", current.a)"); break;
    case PREMODULATE: A1; break;
    case MODULATEALPHA_ADDCOLOR: put(b, "("); A1; put(b, " + "); A1; put(b, ".a * "); A2; put(b, ")"); break;
    case MODULATECOLOR_ADDALPHA: put(b, "("); A1; put(b, " * "); A2; put(b, " + "); A1; put(b, ".a)"); break;
    case MODULATEINVALPHA_ADDCOLOR: put(b, "((1.0 - "); A1; put(b, ".a) * "); A2; put(b, " + "); A1; put(b, ")"); break;
    case MODULATEINVCOLOR_ADDALPHA: put(b, "((1.0 - "); A1; put(b, ") * "); A2; put(b, " + "); A1; put(b, ".a)"); break;
    case DOTPRODUCT3: put(b, "float4(saturate(4.0 * dot(("); A1; put(b, ").rgb - 0.5, ("); A2; put(b, ").rgb - 0.5)))"); break;
    case MULTIPLYADD: put(b, "("); A0; put(b, " + "); A1; put(b, " * "); A2; put(b, ")"); break;
    case LERP: put(b, "mix("); A2; put(b, ", "); A1; put(b, ", "); A0; put(b, ")"); break;
    default: put(b, "current"); break;           /* BUMPENVMAP(LUMINANCE): the colour passes through */
    }
#undef A0
#undef A1
#undef A2
}

static const char *cmp_op(int f) {
    switch (f) {
    case 2: return "<"; case 3: return "=="; case 4: return "<="; case 5: return ">"; case 6: return "!=";
    case 7: return ">="; default: return NULL;
    }
}

static void pixel(struct sb *b, const struct ff_pkey *k, unsigned ntc) {
    static const char *types[] = { NULL, "texture2d<float>", "texturecube<float>", "texture3d<float>" };
    put(b, "fragment float4 fff(VOut in [[stage_in]], constant PU &u [[buffer(0)]]");
    for (int i = 0; i < k->nstages; i++)
        if (k->textype[i]) put(b, ", %s tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", types[k->textype[i]], i, i, i, i);
    put(b, ") {\n  float4 diffuse = in.c0, specular = in.c1, current = diffuse, temp = float4(0.0);\n");
    for (int i = 0; i < k->nstages; i++) {
        int prev = i ? k->colorop[i - 1] : 0;
        if (k->textype[i]) {
            int n = k->tcount[i] ? k->tcount[i] : (k->textype[i] == 1 ? 2 : 3);
            put(b, "  float4 c%d = in.tc%d;\n", i, (unsigned)i < ntc ? i : 0);
            if (k->projected[i]) put(b, "  c%d /= c%d[%d];\n", i, i, n - 1);
            if (k->textype[i] == 1) {
                put(b, "  float2 uv%d = c%d.xy;\n", i, i);
                if (prev == BUMPENVMAP || prev == BUMPENVMAPLUMINANCE)
                    put(b, "  uv%d += float2(u.bumpmat[%d].x * t%d.r + u.bumpmat[%d].z * t%d.g,"
                           " u.bumpmat[%d].y * t%d.r + u.bumpmat[%d].w * t%d.g);\n",
                        i, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1);
                put(b, "  float4 t%d = tex%d.sample(smp%d, uv%d, bias(u.lodbias[%d].x));\n", i, i, i, i, i);
            } else put(b, "  float4 t%d = tex%d.sample(smp%d, c%d.xyz, bias(u.lodbias[%d].x));\n", i, i, i, i, i);
            if (prev == BUMPENVMAPLUMINANCE)
                put(b, "  t%d.rgb *= saturate(t%d.b * u.bumpl[%d].x + u.bumpl[%d].y);\n", i, i - 1, i - 1, i - 1);
        } else put(b, "  float4 t%d = float4(1.0);\n", i);   /* no texture on the stage reads as white */
        put(b, "  {\n    float3 rc = saturate(");
        op_expr(b, k->colorop[i], k->colorarg[i], i);
        put(b, ".rgb);\n");
        if (k->alphaop[i] != DISABLE) {
            put(b, "    float ra = saturate(");
            op_expr(b, k->alphaop[i], k->alphaarg[i], i);
            put(b, ".a);\n");
        } else put(b, "    float ra = current.a;\n");
        if (k->colorop[i] == BUMPENVMAP || k->colorop[i] == BUMPENVMAPLUMINANCE) put(b, "    rc = current.rgb;\n");
        put(b, "    %s = float4(rc, ra);\n  }\n", k->resultarg[i] == 5 ? "temp" : "current");
    }
    if (k->specular_add) put(b, "  current.rgb = saturate(current.rgb + specular.rgb);\n");
    if (k->fog) {
        if (k->fog == 1) put(b, "  float f = in.fog;\n");
        else {
            put(b, "  float fz = in.fogz;\n");
            if (k->fog == 2) put(b, "  float f = exp(-fz * u.fog.z);\n");
            else if (k->fog == 3) put(b, "  float f = exp(-(fz * u.fog.z) * (fz * u.fog.z));\n");
            else put(b, "  float f = (u.fog.y - fz) / (u.fog.y - u.fog.x);\n");
        }
        put(b, "  current.rgb = mix(u.fogcolor.rgb, current.rgb, saturate(f));\n");
    }
    if (k->alpha_func == 1) put(b, "  discard_fragment();\n");
    else if (cmp_op(k->alpha_func))
        put(b, "  if (!(rint(saturate(current.a) * 255.0) %s u.fog.w)) discard_fragment();\n", cmp_op(k->alpha_func));
    if (k->srgb_write)
        put(b, "  current.rgb = select(1.055 * pow(current.rgb, float3(1.0 / 2.4)) - 0.055, current.rgb * 12.92,"
               " current.rgb <= 0.0031308);\n");
    put(b, "  return current;\n}\n");
}

char *ff_msl(const struct ff_vkey *vk, const struct ff_pkey *pk) {
    struct sb b = { 0 };
    put(&b, "%s", HEADER);
    vertex(&b, vk);
    pixel(&b, pk, vk->nstages);
    return b.s;
}

char *ff_msl_fragment(const struct ff_pkey *pk, unsigned ntc) {
    struct sb b = { 0 };
    put(&b, "%s", HEADER);
    vout(&b, ntc, pk->flat, 0, 0);
    pixel(&b, pk, ntc);
    return b.s;
}
