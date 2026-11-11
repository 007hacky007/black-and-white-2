/*
 * sm_decode.c - D3D9 shader bytecode to struct sm_shader (plan 5 Task 2),
 * with the checks that keep the translators to shader models 1.1-2.0:
 * the version, each instruction in its version, register types and ranges,
 * modifiers, instruction lengths, relative addressing. Anything else fails
 * with a message naming the version, the problem and its token offset.
 *
 * The token format is D3D9's: an instruction token (opcode bits 0-15,
 * controls 16-23, length 24-27 from 2.0, predicated bit 28, co-issue bit 30),
 * then parameter tokens (bit 31 set; register number 0-10, type bits 28-30
 * and 11-12, relative bit 13; a destination's write mask 16-19, result
 * modifiers 20-23 and shift 24-27; a source's swizzle 16-23 and modifier
 * 24-27). vs_1_1 relative addressing has no address token (a0.x).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sm.h"

#define ALL 0x03FFu        /* every version up to 3_sw */
#define V11 0x0101u
#define V12 0x0102u
#define V13 0x0103u
#define V14 0x0104u
#define V20 0x0200u
#define V2X 0x0201u
#define V30 0x0300u

/* name, ndst, nsrc, vs range, ps range */
static const struct sm_opinfo g_ops[] = {
    { SM_NOP, "nop", 0, 0, V11, ALL, V11, ALL },
    { SM_MOV, "mov", 1, 1, V11, ALL, V11, ALL },
    { SM_ADD, "add", 1, 2, V11, ALL, V11, ALL },
    { SM_SUB, "sub", 1, 2, V11, ALL, V11, ALL },
    { SM_MAD, "mad", 1, 3, V11, ALL, V11, ALL },
    { SM_MUL, "mul", 1, 2, V11, ALL, V11, ALL },
    { SM_RCP, "rcp", 1, 1, V11, ALL, V20, ALL },
    { SM_RSQ, "rsq", 1, 1, V11, ALL, V20, ALL },
    { SM_DP3, "dp3", 1, 2, V11, ALL, V11, ALL },
    { SM_DP4, "dp4", 1, 2, V11, ALL, V12, ALL },
    { SM_MIN, "min", 1, 2, V11, ALL, V20, ALL },
    { SM_MAX, "max", 1, 2, V11, ALL, V20, ALL },
    { SM_SLT, "slt", 1, 2, V11, ALL, 0, 0 },
    { SM_SGE, "sge", 1, 2, V11, ALL, 0, 0 },
    { SM_EXP, "exp", 1, 1, V11, ALL, V20, ALL },
    { SM_LOG, "log", 1, 1, V11, ALL, V20, ALL },
    { SM_LIT, "lit", 1, 1, V11, ALL, 0, 0 },
    { SM_DST, "dst", 1, 2, V11, ALL, 0, 0 },
    { SM_LRP, "lrp", 1, 3, V20, ALL, V11, ALL },
    { SM_FRC, "frc", 1, 1, V11, ALL, V20, ALL },
    { SM_M4x4, "m4x4", 1, 2, V11, ALL, V20, ALL },
    { SM_M4x3, "m4x3", 1, 2, V11, ALL, V20, ALL },
    { SM_M3x4, "m3x4", 1, 2, V11, ALL, V20, ALL },
    { SM_M3x3, "m3x3", 1, 2, V11, ALL, V20, ALL },
    { SM_M3x2, "m3x2", 1, 2, V11, ALL, V20, ALL },
    { SM_CALL, "call", 0, 1, V20, ALL, V2X, ALL },
    { SM_CALLNZ, "callnz", 0, 2, V20, ALL, V2X, ALL },
    { SM_LOOP, "loop", 0, 2, V20, ALL, V30, ALL },
    { SM_RET, "ret", 0, 0, V20, ALL, V2X, ALL },
    { SM_ENDLOOP, "endloop", 0, 0, V20, ALL, V30, ALL },
    { SM_LABEL, "label", 0, 1, V20, ALL, V2X, ALL },
    { SM_DCL, "dcl", 1, 0, V11, ALL, V20, ALL },
    { SM_POW, "pow", 1, 2, V20, ALL, V20, ALL },
    { SM_CRS, "crs", 1, 2, V20, ALL, V20, ALL },
    { SM_SGN, "sgn", 1, 3, V20, V2X, 0, 0 },
    { SM_SGN, "sgn", 1, 1, V30, ALL, 0, 0 },
    { SM_ABS, "abs", 1, 1, V20, ALL, V20, ALL },
    { SM_NRM, "nrm", 1, 1, V20, ALL, V20, ALL },
    { SM_SINCOS, "sincos", 1, 3, V20, V2X, V20, V2X },
    { SM_SINCOS, "sincos", 1, 1, V30, ALL, V30, ALL },
    { SM_REP, "rep", 0, 1, V20, ALL, V2X, ALL },
    { SM_ENDREP, "endrep", 0, 0, V20, ALL, V2X, ALL },
    { SM_IF, "if", 0, 1, V20, ALL, V2X, ALL },
    { SM_IFC, "ifc", 0, 2, V2X, ALL, V2X, ALL },
    { SM_ELSE, "else", 0, 0, V20, ALL, V2X, ALL },
    { SM_ENDIF, "endif", 0, 0, V20, ALL, V2X, ALL },
    { SM_BREAK, "break", 0, 0, V2X, ALL, V2X, ALL },
    { SM_BREAKC, "breakc", 0, 2, V2X, ALL, V2X, ALL },
    { SM_MOVA, "mova", 1, 1, V20, ALL, 0, 0 },
    { SM_DEFB, "defb", 1, 0, V20, ALL, V2X, ALL },
    { SM_DEFI, "defi", 1, 0, V20, ALL, V2X, ALL },
    { SM_TEXCOORD, "texcoord", 1, 0, 0, 0, V11, V13 },
    { SM_TEXCOORD, "texcrd", 1, 1, 0, 0, V14, V14 },
    { SM_TEXKILL, "texkill", 1, 0, 0, 0, V11, ALL },
    { SM_TEX, "tex", 1, 0, 0, 0, V11, V13 },
    { SM_TEX, "texld", 1, 1, 0, 0, V14, V14 },
    { SM_TEX, "texld", 1, 2, 0, 0, V20, ALL },
    { SM_TEXBEM, "texbem", 1, 1, 0, 0, V11, V13 },
    { SM_TEXBEML, "texbeml", 1, 1, 0, 0, V11, V13 },
    { SM_TEXREG2AR, "texreg2ar", 1, 1, 0, 0, V11, V13 },
    { SM_TEXREG2GB, "texreg2gb", 1, 1, 0, 0, V11, V13 },
    { SM_TEXM3x2PAD, "texm3x2pad", 1, 1, 0, 0, V11, V13 },
    { SM_TEXM3x2TEX, "texm3x2tex", 1, 1, 0, 0, V11, V13 },
    { SM_TEXM3x3PAD, "texm3x3pad", 1, 1, 0, 0, V11, V13 },
    { SM_TEXM3x3TEX, "texm3x3tex", 1, 1, 0, 0, V11, V13 },
    { SM_TEXM3x3SPEC, "texm3x3spec", 1, 2, 0, 0, V11, V13 },
    { SM_TEXM3x3VSPEC, "texm3x3vspec", 1, 1, 0, 0, V11, V13 },
    { SM_EXPP, "expp", 1, 1, V11, ALL, 0, 0 },
    { SM_LOGP, "logp", 1, 1, V11, ALL, 0, 0 },
    { SM_CND, "cnd", 1, 3, 0, 0, V11, V14 },
    { SM_DEF, "def", 1, 0, V11, ALL, V11, ALL },
    { SM_TEXREG2RGB, "texreg2rgb", 1, 1, 0, 0, V12, V13 },
    { SM_TEXDP3TEX, "texdp3tex", 1, 1, 0, 0, V12, V13 },
    { SM_TEXM3x2DEPTH, "texm3x2depth", 1, 1, 0, 0, V13, V13 },
    { SM_TEXDP3, "texdp3", 1, 1, 0, 0, V12, V13 },
    { SM_TEXM3x3, "texm3x3", 1, 1, 0, 0, V12, V13 },
    { SM_TEXDEPTH, "texdepth", 1, 0, 0, 0, V14, V14 },
    { SM_CMP, "cmp", 1, 3, 0, 0, V12, ALL },
    { SM_BEM, "bem", 1, 2, 0, 0, V14, V14 },
    { SM_DP2ADD, "dp2add", 1, 3, 0, 0, V20, ALL },
    { SM_DSX, "dsx", 1, 1, 0, 0, V2X, ALL },
    { SM_DSY, "dsy", 1, 1, 0, 0, V2X, ALL },
    { SM_TEXLDD, "texldd", 1, 4, 0, 0, V2X, ALL },
    { SM_SETP, "setp", 1, 2, V2X, ALL, V2X, ALL },
    { SM_TEXLDL, "texldl", 1, 2, V30, ALL, V30, ALL },
    { SM_BREAKP, "breakp", 0, 1, V2X, ALL, V2X, ALL },
    { SM_PHASE, "phase", 0, 0, 0, 0, V14, V14 },
};
#define NOPS (sizeof g_ops / sizeof *g_ops)

static int in_range(const struct sm_opinfo *o, uint32_t version) {
    uint32_t v = version & 0xFFFFu;
    return SM_IS_PS(version) ? o->ps_lo && v >= o->ps_lo && v <= o->ps_hi : o->vs_lo && v >= o->vs_lo && v <= o->vs_hi;
}
const struct sm_opinfo *sm_opinfo(uint16_t op, uint32_t version) {
    for (size_t i = 0; i < NOPS; i++) if (g_ops[i].op == op && in_range(&g_ops[i], version)) return &g_ops[i];
    return NULL;
}
const struct sm_opinfo *sm_opinfo_named(const char *name, uint32_t version) {
    for (size_t i = 0; i < NOPS; i++) if (!strcmp(g_ops[i].name, name) && in_range(&g_ops[i], version)) return &g_ops[i];
    return NULL;
}
int sm_op_known(const char *name) {
    for (size_t i = 0; i < NOPS; i++) if (!strcmp(g_ops[i].name, name)) return 1;
    return 0;
}
static const char *op_anyname(uint16_t op, uint32_t version) {
    const char *best = NULL;
    for (size_t i = 0; i < NOPS; i++)
        if (g_ops[i].op == op) {
            /* the name the shader type would use: texld for ps 1.4 up, tex before */
            if (!best || (SM_IS_PS(version) ? g_ops[i].ps_lo : g_ops[i].vs_lo)) best = g_ops[i].name;
            if (SM_IS_PS(version) ? g_ops[i].ps_lo : g_ops[i].vs_lo) break;
        }
    return best;
}

const char *sm_version_name(uint32_t version, char *buf, size_t n) {
    uint32_t major = (version >> 8) & 0xFF, minor = version & 0xFF, type = version >> 16;
    const char *t = type == 0xFFFEu ? "vs" : type == 0xFFFFu ? "ps" : NULL;
    if (!t) snprintf(buf, n, "version 0x%08X", version);
    else if (major == 2 && minor == 1) snprintf(buf, n, "%s_2_x", t);
    else if (minor == 0xFF) snprintf(buf, n, "%s_%u_sw", t, major);
    else snprintf(buf, n, "%s_%u_%u", t, major, minor);
    return buf;
}

const char *sm_reg_name(uint32_t version, const struct sm_reg *r, char *buf, size_t n) {
    unsigned i = r->index;
    switch (r->type) {
    case SM_R_TEMP: snprintf(buf, n, "r%u", i); break;
    case SM_R_INPUT: snprintf(buf, n, "v%u", i); break;
    case SM_R_CONST: snprintf(buf, n, "c%u", i); break;
    case SM_R_ADDR: snprintf(buf, n, SM_IS_PS(version) ? "t%u" : "a%u", i); break;
    case SM_R_RASTOUT: snprintf(buf, n, i == 0 ? "oPos" : i == 1 ? "oFog" : i == 2 ? "oPts" : "oRast%u", i); break;
    case SM_R_ATTROUT: snprintf(buf, n, "oD%u", i); break;
    case SM_R_TEXCRDOUT: snprintf(buf, n, ((version >> 8) & 0xFF) >= 3 ? "o%u" : "oT%u", i); break;
    case SM_R_CONSTINT: snprintf(buf, n, "i%u", i); break;
    case SM_R_COLOROUT: snprintf(buf, n, "oC%u", i); break;
    case SM_R_DEPTHOUT: snprintf(buf, n, "oDepth"); break;
    case SM_R_SAMPLER: snprintf(buf, n, "s%u", i); break;
    case SM_R_CONST2: case SM_R_CONST3: case SM_R_CONST4:
        snprintf(buf, n, "c%u", i + 2048u * (unsigned)(r->type - SM_R_CONST2 + 1)); break;
    case SM_R_CONSTBOOL: snprintf(buf, n, "b%u", i); break;
    case SM_R_LOOP: snprintf(buf, n, "aL"); break;
    case SM_R_TEMPFLOAT16: snprintf(buf, n, "half%u", i); break;
    case SM_R_MISCTYPE: snprintf(buf, n, i == 0 ? "vPos" : i == 1 ? "vFace" : "misc%u", i); break;
    case SM_R_LABEL: snprintf(buf, n, "l%u", i); break;
    case SM_R_PREDICATE: snprintf(buf, n, "p%u", i); break;
    default: snprintf(buf, n, "register type %u #%u", r->type, i); break;
    }
    return buf;
}

uint64_t sm_hash(const uint32_t *tokens, uint32_t ntokens) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t *b = (const uint8_t *)tokens;
    for (size_t k = 0; k < 4u * (size_t)ntokens; k++) h = (h ^ b[k]) * 0x100000001B3ull;
    return h;
}

void sm_free(struct sm_shader *s) {
    free(s->ins);
    s->ins = NULL, s->nins = 0;
}

/* ---- the decoder ---- */

struct dec {
    const uint32_t *t;
    uint32_t n, pos, ver, vnum;
    int ps;
    char *err;
    size_t errlen;
    uint32_t *bad;
    char vname[16];
};
enum { ROLE_SRC, ROLE_DST, ROLE_DCL, ROLE_DEF };

static int fail(struct dec *d, uint32_t at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(struct dec *d, uint32_t at, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(d->err, d->errlen, "%s: %s (token %u)", d->vname, msg, at);
    if (d->bad) *d->bad = at;
    return -1;
}

static int next(struct dec *d, uint32_t *tok) {
    if (d->pos >= d->n) return fail(d, d->pos, "the shader ends inside an instruction (no end token)");
    *tok = d->t[d->pos++];
    return 0;
}

/* Is register type/index valid in this version for role? 0, or -1 (message set). */
static int check_reg(struct dec *d, uint32_t at, const struct sm_reg *r, int role) {
    uint32_t v = d->vnum, limit = 0;
    int ok_role = 0;
    if (!d->ps) {
        switch (r->type) {
        case SM_R_TEMP: limit = 12, ok_role = role == ROLE_SRC || role == ROLE_DST; break;
        case SM_R_INPUT: limit = 16, ok_role = role == ROLE_SRC || role == ROLE_DCL; break;
        case SM_R_CONST: limit = 256, ok_role = role == ROLE_SRC || role == ROLE_DEF; break;
        case SM_R_ADDR: limit = 1, ok_role = role == ROLE_DST; break;
        case SM_R_RASTOUT: limit = 3, ok_role = role == ROLE_DST; break;
        case SM_R_ATTROUT: limit = 2, ok_role = role == ROLE_DST; break;
        case SM_R_TEXCRDOUT: limit = 8, ok_role = role == ROLE_DST; break;
        case SM_R_CONSTINT: case SM_R_CONSTBOOL:
            limit = 16, ok_role = v >= V20 && (role == ROLE_SRC || role == ROLE_DEF); break;
        case SM_R_LOOP: limit = 1, ok_role = v >= V20 && role == ROLE_SRC; break;
        case SM_R_LABEL: limit = 2048, ok_role = v >= V20 && role == ROLE_SRC; break;
        }
    } else if (v <= V13) {
        switch (r->type) {
        case SM_R_TEMP: limit = 2, ok_role = role == ROLE_SRC || role == ROLE_DST; break;
        case SM_R_INPUT: limit = 2, ok_role = role == ROLE_SRC; break;
        case SM_R_CONST: limit = 8, ok_role = role == ROLE_SRC || role == ROLE_DEF; break;
        case SM_R_TEXTURE: limit = 4, ok_role = role == ROLE_SRC || role == ROLE_DST; break;
        }
    } else if (v == V14) {
        switch (r->type) {
        case SM_R_TEMP: limit = 6, ok_role = role == ROLE_SRC || role == ROLE_DST; break;
        case SM_R_INPUT: limit = 2, ok_role = role == ROLE_SRC; break;
        case SM_R_CONST: limit = 8, ok_role = role == ROLE_SRC || role == ROLE_DEF; break;
        case SM_R_TEXTURE: limit = 6, ok_role = role == ROLE_SRC; break;
        }
    } else {
        switch (r->type) {
        case SM_R_TEMP: limit = 12, ok_role = role == ROLE_SRC || role == ROLE_DST; break;
        case SM_R_INPUT: limit = 2, ok_role = role == ROLE_SRC || role == ROLE_DCL; break;
        case SM_R_CONST: limit = 32, ok_role = role == ROLE_SRC || role == ROLE_DEF; break;
        case SM_R_TEXTURE: limit = 8, ok_role = role == ROLE_SRC || role == ROLE_DCL; break;
        case SM_R_SAMPLER: limit = 16, ok_role = role == ROLE_SRC || role == ROLE_DCL; break;
        case SM_R_COLOROUT: limit = 4, ok_role = role == ROLE_DST; break;
        case SM_R_DEPTHOUT: limit = 1, ok_role = role == ROLE_DST; break;
        }
    }
    char nb[32];
    sm_reg_name(d->ver, r, nb, sizeof nb);
    static const char *const roles[] = { "a source", "a destination", "a declaration", "a definition" };
    if (!ok_role) return fail(d, at, "register %s is not valid as %s", nb, roles[role]);
    if (r->index >= limit) return fail(d, at, "register %s is out of range", nb);
    return 0;
}

static void reg_fields(uint32_t tok, struct sm_reg *r) {
    r->type = (uint8_t)(((tok >> 28) & 7u) | ((tok >> 8) & 0x18u));
    r->index = (uint16_t)(tok & 0x7FFu);
}

static const char *const g_srcmod[] = { "", "negate", "_bias", "-_bias", "_bx2", "-_bx2", "1-", "_x2", "-_x2",
                                        "_dz", "_dw", "_abs", "-_abs", "!" };
static const char *const g_shift[] = { "", "_x2", "_x4", "_x8", "shift 4", "shift 5", "shift 6", "shift 7",
                                       "shift 8", "shift 9", "shift 10", "shift 11", "shift 12", "_d8", "_d4", "_d2" };

static int read_dst(struct dec *d, struct sm_reg *r, int role) {
    uint32_t at = d->pos, tok;
    if (next(d, &tok)) return -1;
    if (!(tok & 0x80000000u)) return fail(d, at, "malformed parameter token 0x%08X", tok);
    reg_fields(tok, r);
    r->mask_or_swizzle = (uint8_t)((tok >> 16) & 0xF);
    r->mod = (uint8_t)((tok >> 20) & 0xF);
    r->shift = (int8_t)(((int32_t)(tok << 4)) >> 28);
    if (tok & 0x2000u) return fail(d, at, "relative addressing on a destination");
    if (role == ROLE_DST || role == ROLE_DCL) {
        if (r->mod & SM_DST_SAT && !d->ps) return fail(d, at, "result modifier _sat is not valid");
        if (r->mod & SM_DST_PP && !(d->ps && d->vnum >= V20)) return fail(d, at, "result modifier _pp is not valid");
        if (r->mod & SM_DST_CENTROID && !(d->ps && d->vnum >= V20))
            return fail(d, at, "result modifier _centroid is not valid");
        if (r->mod & 8u) return fail(d, at, "result modifier 0x8 is not valid");
        int s = r->shift;
        int ok = s == 0 || (d->ps && d->vnum <= V13 && (s == 1 || s == 2 || s == -1)) ||
                 (d->ps && d->vnum == V14 && s >= -3 && s <= 3);
        if (!ok) return fail(d, at, "result shift %s is not valid", g_shift[(tok >> 24) & 0xF]);
        if (!r->mask_or_swizzle) return fail(d, at, "empty write mask");
    } else if (r->mod || r->shift) return fail(d, at, "modifiers on a definition or declaration");
    return check_reg(d, at, r, role);
}

static int read_src(struct dec *d, struct sm_reg *r) {
    uint32_t at = d->pos, tok;
    if (next(d, &tok)) return -1;
    if (!(tok & 0x80000000u)) return fail(d, at, "malformed parameter token 0x%08X", tok);
    reg_fields(tok, r);
    r->mask_or_swizzle = (uint8_t)((tok >> 16) & 0xFF);
    r->mod = (uint8_t)((tok >> 24) & 0xF);
    r->shift = 0;
    int m = r->mod, okm;
    if (!d->ps || d->vnum >= V20) okm = m == SM_SRC_NONE || m == SM_SRC_NEG;
    else if (d->vnum <= V13) okm = m <= SM_SRC_COMP;
    else okm = m <= SM_SRC_DW;
    if (!okm) return fail(d, at, "source modifier %s is not valid", m < 14 ? g_srcmod[m] : "14/15");
    if (tok & 0x2000u) {
        if (d->ps) return fail(d, at, "relative addressing is not valid in a pixel shader");
        if (r->type != SM_R_CONST) return fail(d, at, "relative addressing of a register that is not a constant");
        r->rel = 1;
        if (d->vnum < V20) r->rel_type = SM_R_ADDR, r->rel_index = 0, r->rel_comp = 0;
        else {
            uint32_t rat = d->pos, rt;
            if (next(d, &rt)) return -1;
            if (!(rt & 0x80000000u)) return fail(d, rat, "malformed relative address token 0x%08X", rt);
            struct sm_reg a;
            reg_fields(rt, &a);
            if ((a.type != SM_R_ADDR && a.type != SM_R_LOOP) || a.index != 0)
                return fail(d, rat, "relative address register type %u #%u is not a0 or aL", a.type, a.index);
            if (rt & 0x0F002000u) return fail(d, rat, "modifiers on a relative address token");
            r->rel_type = a.type, r->rel_index = a.index, r->rel_comp = (uint8_t)((rt >> 16) & 3);
        }
    } else r->rel = 0, r->rel_type = 0, r->rel_index = 0, r->rel_comp = 0;
    return check_reg(d, at, r, ROLE_SRC);
}

static int decode_ins(struct dec *d, struct sm_ins *in, uint32_t tok, int first) {
    uint32_t start = d->pos - 1, len = (tok >> 24) & 0xF;
    uint16_t op = (uint16_t)(tok & 0xFFFF);
    const struct sm_opinfo *oi = sm_opinfo(op, d->ver);
    if (!oi) {
        const char *nm = op_anyname(op, d->ver);
        if (nm) return fail(d, start, "instruction %s is not supported", nm);
        return fail(d, start, "unknown opcode %u", op);
    }
    memset(in, 0, sizeof *in);
    in->op = op, in->offset = start, in->ndst = oi->ndst, in->nsrc = oi->nsrc;
    if (tok & 0x80000000u) return fail(d, start, "malformed instruction token 0x%08X", tok);
    if (tok & 0x10000000u) return fail(d, start, "predicated instructions are not supported");
    if (tok & 0x20000000u) return fail(d, start, "reserved bit 29 set in instruction %s", oi->name);
    if (tok & 0x40000000u) {
        if (!d->ps || d->vnum >= V20) return fail(d, start, "co-issue is not valid");
        if (first) return fail(d, start, "co-issue without an instruction before it");
        in->coissue = 1;
    }
    in->control = (uint8_t)((tok >> 16) & 0xFF);
    if (in->control && !(op == SM_TEX && d->ps && d->vnum >= V20 && in->control <= SM_TEXLD_BIAS))
        return fail(d, start, "instruction %s with controls 0x%X is not supported", oi->name, in->control);
    if (op == SM_DCL) {
        uint32_t at = d->pos, dt;
        if (next(d, &dt)) return -1;
        if (!(dt & 0x80000000u)) return fail(d, at, "malformed declaration token 0x%08X", dt);
        in->usage = dt & 0x1Fu, in->usage_index = (dt >> 16) & 0xFu, in->sampler_type = (dt >> 27) & 0xFu;
        if (read_dst(d, &in->dst, ROLE_DCL)) return -1;
        if (in->dst.type == SM_R_SAMPLER) {
            if (in->sampler_type < SM_STT_2D || in->sampler_type > SM_STT_VOLUME)
                return fail(d, at, "sampler type %u is not 2d, cube or volume", in->sampler_type);
            if (dt & 0x07FFFFFFu) return fail(d, at, "malformed sampler declaration token 0x%08X", dt);
        } else {
            if (dt & 0x7FF0FFE0u) return fail(d, at, "malformed declaration token 0x%08X", dt);
            if (in->usage > SM_USAGE_SAMPLE) return fail(d, at, "declaration usage %u is not valid", in->usage);
        }
        in->pp = (in->dst.mod & SM_DST_PP) != 0, in->centroid = (in->dst.mod & SM_DST_CENTROID) != 0;
        in->dst.mod = 0;
    } else if (op == SM_DEF || op == SM_DEFI || op == SM_DEFB) {
        if (read_dst(d, &in->dst, ROLE_DEF)) return -1;
        uint8_t want = op == SM_DEF ? SM_R_CONST : op == SM_DEFI ? SM_R_CONSTINT : SM_R_CONSTBOOL;
        if (in->dst.type != want) {
            char nb[32];
            return fail(d, start, "%s of register %s", oi->name, sm_reg_name(d->ver, &in->dst, nb, sizeof nb));
        }
        for (int k = 0; k < (op == SM_DEFB ? 1 : 4); k++) {
            uint32_t v;
            if (next(d, &v)) return -1;
            if (op == SM_DEF) memcpy(&in->def[k], &v, 4);
            else in->defi[k] = (int32_t)v;
        }
    } else {
        if (oi->ndst) {
            /* texkill's parameter is a destination token naming the register it reads */
            if (read_dst(d, &in->dst, op == SM_TEXKILL ? ROLE_SRC : ROLE_DST)) return -1;
            if (op == SM_TEXKILL && (in->dst.mod || in->dst.shift)) return fail(d, start, "modifiers on texkill");
            in->sat = (in->dst.mod & SM_DST_SAT) != 0, in->pp = (in->dst.mod & SM_DST_PP) != 0;
            in->centroid = (in->dst.mod & SM_DST_CENTROID) != 0;
        }
        for (int k = 0; k < oi->nsrc; k++) if (read_src(d, &in->src[k])) return -1;
    }
    uint32_t got = d->pos - start - 1;
    in->length = (uint8_t)(got + 1);
    if (d->vnum >= V20 ? len != got : len != 0 && len != got)
        return fail(d, start, "instruction %s has length %u, its parameters take %u", oi->name, len, got);
    return 0;
}

int sm_decode_at(const uint32_t *tokens, uint32_t max_tokens, struct sm_shader *out, char *err, size_t errlen,
                 uint32_t *bad) {
    struct dec d = { .t = tokens, .n = max_tokens, .err = err, .errlen = errlen, .bad = bad };
    memset(out, 0, sizeof *out);
    if (bad) *bad = 0;
    if (!tokens || !max_tokens) { snprintf(err, errlen, "no shader tokens"); return -1; }
    d.ver = tokens[0], d.vnum = d.ver & 0xFFFFu, d.ps = SM_IS_PS(d.ver);
    sm_version_name(d.ver, d.vname, sizeof d.vname);
    uint32_t type = d.ver >> 16;
    if (type != 0xFFFEu && type != 0xFFFFu) {
        snprintf(err, errlen, "0x%08X is not a shader version token", d.ver);
        return -1;
    }
    int supported = d.ps ? (d.vnum >= V11 && d.vnum <= V14) || d.vnum == V20 : d.vnum == V11 || d.vnum == V20;
    if (!supported) {
        snprintf(err, errlen, "%s is not supported (vs_1_1, vs_2_0, ps_1_1 to ps_1_4 and ps_2_0 are)", d.vname);
        return -1;
    }
    uint32_t cap = 64, n = 0;
    struct sm_ins *ins = malloc(sizeof *ins * cap);
    d.pos = 1;
    for (;;) {
        uint32_t at = d.pos, tok;
        if (next(&d, &tok)) goto bad;
        if (tok == 0x0000FFFFu) break;
        if ((tok & 0xFFFFu) == SM_COMMENT) {
            if (tok & 0x80000000u) { fail(&d, at, "malformed comment token 0x%08X", tok); goto bad; }
            uint32_t len = (tok >> 16) & 0x7FFFu;
            if (len > d.n - d.pos) { fail(&d, at, "a comment runs past the end"); goto bad; }
            d.pos += len;
            continue;
        }
        if (n == cap) ins = realloc(ins, sizeof *ins * (cap *= 2));
        if (decode_ins(&d, &ins[n], tok, n == 0)) goto bad;
        n++;
    }
    out->version = d.ver, out->nins = n, out->ins = ins, out->ntokens = d.pos;
    out->hash = sm_hash(tokens, d.pos);
    out->is_ps = (uint8_t)d.ps, out->major = (uint8_t)(d.vnum >> 8), out->minor = (uint8_t)(d.vnum & 0xFF);
    return 0;
bad:
    free(ins);
    return -1;
}

int sm_decode(const uint32_t *tokens, uint32_t max_tokens, struct sm_shader *out, char *err, size_t errlen) {
    return sm_decode_at(tokens, max_tokens, out, err, errlen, NULL);
}
