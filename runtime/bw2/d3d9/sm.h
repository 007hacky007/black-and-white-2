/*
 * sm.h - D3D9 shader bytecode (plan 5): the token format, the decoded form
 * the translators read (Tasks 3-6), the decoder (sm_decode.c) and the
 * assembler (d3dx/d3dx_asm.c). Shader models vs_1_1, vs_2_0, ps_1_1-ps_1_4
 * and ps_2_0 are accepted; everything else fails, naming what it is.
 */
#ifndef SM_H
#define SM_H
#include <stddef.h>
#include <stdint.h>

/* D3DSHADER_INSTRUCTION_OPCODE_TYPE */
enum {
    SM_NOP, SM_MOV, SM_ADD, SM_SUB, SM_MAD, SM_MUL, SM_RCP, SM_RSQ, SM_DP3, SM_DP4, SM_MIN, SM_MAX, SM_SLT,
    SM_SGE, SM_EXP, SM_LOG, SM_LIT, SM_DST, SM_LRP, SM_FRC, SM_M4x4, SM_M4x3, SM_M3x4, SM_M3x3, SM_M3x2,
    SM_CALL, SM_CALLNZ, SM_LOOP, SM_RET, SM_ENDLOOP, SM_LABEL, SM_DCL, SM_POW, SM_CRS, SM_SGN, SM_ABS, SM_NRM,
    SM_SINCOS, SM_REP, SM_ENDREP, SM_IF, SM_IFC, SM_ELSE, SM_ENDIF, SM_BREAK, SM_BREAKC, SM_MOVA, SM_DEFB,
    SM_DEFI,
    SM_TEXCOORD = 64, SM_TEXKILL, SM_TEX, SM_TEXBEM, SM_TEXBEML, SM_TEXREG2AR, SM_TEXREG2GB, SM_TEXM3x2PAD,
    SM_TEXM3x2TEX, SM_TEXM3x3PAD, SM_TEXM3x3TEX, SM_RESERVED0, SM_TEXM3x3SPEC, SM_TEXM3x3VSPEC, SM_EXPP,
    SM_LOGP, SM_CND, SM_DEF, SM_TEXREG2RGB, SM_TEXDP3TEX, SM_TEXM3x2DEPTH, SM_TEXDP3, SM_TEXM3x3, SM_TEXDEPTH,
    SM_CMP, SM_BEM, SM_DP2ADD, SM_DSX, SM_DSY, SM_TEXLDD, SM_SETP, SM_TEXLDL, SM_BREAKP,
    SM_PHASE = 0xFFFD, SM_COMMENT = 0xFFFE, SM_END = 0xFFFF,
};

/* D3DSHADER_PARAM_REGISTER_TYPE (ADDR and TEXTURE share 3, TEXCRDOUT and OUTPUT 6) */
enum {
    SM_R_TEMP, SM_R_INPUT, SM_R_CONST, SM_R_ADDR, SM_R_TEXTURE = 3, SM_R_RASTOUT, SM_R_ATTROUT, SM_R_TEXCRDOUT,
    SM_R_CONSTINT, SM_R_COLOROUT, SM_R_DEPTHOUT, SM_R_SAMPLER, SM_R_CONST2, SM_R_CONST3, SM_R_CONST4,
    SM_R_CONSTBOOL, SM_R_LOOP, SM_R_TEMPFLOAT16, SM_R_MISCTYPE, SM_R_LABEL, SM_R_PREDICATE,
};
enum { SM_RAST_POSITION, SM_RAST_FOG, SM_RAST_POINTSIZE };

/* D3DSHADER_PARAM_SRCMOD_TYPE */
enum {
    SM_SRC_NONE, SM_SRC_NEG, SM_SRC_BIAS, SM_SRC_BIASNEG, SM_SRC_SIGN, SM_SRC_SIGNNEG, SM_SRC_COMP, SM_SRC_X2,
    SM_SRC_X2NEG, SM_SRC_DZ, SM_SRC_DW, SM_SRC_ABS, SM_SRC_ABSNEG, SM_SRC_NOT,
};
/* result modifiers (dst mod) */
#define SM_DST_SAT       1u
#define SM_DST_PP        2u
#define SM_DST_CENTROID  4u
/* D3DDECLUSAGE */
enum {
    SM_USAGE_POSITION, SM_USAGE_BLENDWEIGHT, SM_USAGE_BLENDINDICES, SM_USAGE_NORMAL, SM_USAGE_PSIZE,
    SM_USAGE_TEXCOORD, SM_USAGE_TANGENT, SM_USAGE_BINORMAL, SM_USAGE_TESSFACTOR, SM_USAGE_POSITIONT,
    SM_USAGE_COLOR, SM_USAGE_FOG, SM_USAGE_DEPTH, SM_USAGE_SAMPLE,
};
/* D3DSAMPLER_TEXTURE_TYPE >> 27 */
enum { SM_STT_UNKNOWN, SM_STT_2D = 2, SM_STT_CUBE, SM_STT_VOLUME };
/* the texld controls (bits 16-23 of the instruction token) */
#define SM_TEXLD_PROJECT 1u
#define SM_TEXLD_BIAS    2u

#define SM_SWIZZLE_IDENTITY 0xE4u
#define SM_VS(major, minor) (0xFFFE0000u | (major) << 8 | (minor))
#define SM_PS(major, minor) (0xFFFF0000u | (major) << 8 | (minor))
#define SM_IS_PS(version) (((version) >> 16) == 0xFFFFu)

/* One parameter. Destinations: mask_or_swizzle is the write mask (bit 0 = x), mod the result modifiers
 * (SM_DST_*), shift -3..3 (_d8.._x8). Sources: an 8-bit swizzle (2 bits per component, x first) and a
 * SM_SRC_* modifier. rel: relative addressing through rel_type/rel_index, component rel_comp
 * (vs_1_1's implied a0.x included). */
struct sm_reg {
    uint8_t type;
    uint16_t index;
    uint8_t mask_or_swizzle;
    uint8_t mod;
    int8_t shift;
    uint8_t rel;
    uint8_t rel_type;
    uint16_t rel_index;
    uint8_t rel_comp;
};

/* One instruction. dcl: usage/usage_index for inputs, sampler_type (SM_STT_*) for samplers, the register
 * in dst. def: def[]; defi: defi[]; defb: defi[0]. control: the texld controls (SM_TEXLD_*).
 * offset: the instruction token's index, for messages; length: its tokens, the instruction token included
 * (a comment after it not). */
struct sm_ins {
    uint16_t op;
    uint8_t ndst, nsrc;
    struct sm_reg dst, src[4];
    uint8_t coissue, sat, pp;
    uint32_t usage, usage_index, sampler_type;
    float def[4];
    int32_t defi[4];
    uint32_t offset;
    uint8_t control, centroid, length;
};

struct sm_shader {
    uint32_t version;                       /* the version token */
    uint32_t nins;
    struct sm_ins *ins;                     /* comments are dropped, the end token is not an instruction */
    uint64_t hash;                          /* FNV-1a 64 of the tokens (bytes, through the end token) */
    uint32_t ntokens;                       /* through the end token */
    uint8_t is_ps, major, minor;
};

/* An instruction's name and parameter counts in a shader version ("texld" is SM_TEX in ps_1_4 and 2.0,
 * "tex" before). NULL: not an instruction of that version. */
struct sm_opinfo {
    uint16_t op;
    const char *name;
    uint8_t ndst, nsrc;
    uint16_t vs_lo, vs_hi, ps_lo, ps_hi;    /* versions as 0xMMmm; 0 = never */
};
const struct sm_opinfo *sm_opinfo(uint16_t op, uint32_t version);
const struct sm_opinfo *sm_opinfo_named(const char *name, uint32_t version);
int sm_op_known(const char *name);                       /* an instruction of some version */
/* "vs_1_1", "ps_2_x", "vs_3_0" ... into buf */
const char *sm_version_name(uint32_t version, char *buf, size_t n);
/* A register as the assembly writes it ("r3", "oPos", "aL", "c" for a relative constant base). */
const char *sm_reg_name(uint32_t version, const struct sm_reg *r, char *buf, size_t n);

/* tokens to *out: 0, or -1 with a message naming the version, the problem and its token offset. */
int sm_decode(const uint32_t *tokens, uint32_t max_tokens, struct sm_shader *out, char *err, size_t errlen);
/* As sm_decode; *bad gets the failing token's index (0 for the version). */
int sm_decode_at(const uint32_t *tokens, uint32_t max_tokens, struct sm_shader *out, char *err, size_t errlen,
                 uint32_t *bad);
void sm_free(struct sm_shader *s);
uint64_t sm_hash(const uint32_t *tokens, uint32_t ntokens);

/* The assembler. defines ends with a NULL name. On success 0, *tokens (malloc'd, through the end
 * token, no comments) and *ntokens; *errors NULL. On failure -1 and *errors (malloc'd) holds
 * "(line): error: <what>" lines. #include stops the run naming the file. */
struct sm_define { const char *name, *value; };
int sm_assemble(const char *text, size_t len, const struct sm_define *defines, uint32_t **tokens,
                uint32_t *ntokens, char **errors);

#endif
