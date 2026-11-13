/*
 * fx.h - D3DX effects (plan 5 Tasks 5-6): the fx_2_0 binary parsed into host
 * structures, its parameters, techniques, passes and states, and effect pools.
 *
 * Every parameter, annotation, technique and pass (and each array element and
 * structure member) has a handle: the guest address of a 4-byte record in a
 * block the effect owns. A value is kept as declared: numbers as dwords (a
 * matrix row- or column-major as its class says), and a guest pointer for
 * each texture, string or shader.
 */
#ifndef FX_H
#define FX_H
#include <stddef.h>
#include <stdint.h>

/* D3DXPARAMETER_CLASS */
enum { FXC_SCALAR, FXC_VECTOR, FXC_MATRIX_ROWS, FXC_MATRIX_COLUMNS, FXC_OBJECT, FXC_STRUCT };
/* D3DXPARAMETER_TYPE */
enum { FXT_VOID, FXT_BOOL, FXT_INT, FXT_FLOAT, FXT_STRING, FXT_TEXTURE, FXT_TEXTURE1D, FXT_TEXTURE2D,
       FXT_TEXTURE3D, FXT_TEXTURECUBE, FXT_SAMPLER, FXT_SAMPLER1D, FXT_SAMPLER2D, FXT_SAMPLER3D,
       FXT_SAMPLERCUBE, FXT_PIXELSHADER, FXT_VERTEXSHADER, FXT_PIXELFRAGMENT, FXT_VERTEXFRAGMENT,
       FXT_UNSUPPORTED };
/* D3DXPARAMETER_DESC.Flags */
#define FX_SHARED      1u
#define FX_LITERAL     2u
#define FX_ANNOTATION  4u
/* a resource's usage: what the state's value comes from */
enum { FX_USAGE_NONE = 0xFF, FX_USAGE_BLOB = 0, FX_USAGE_PARAM = 1, FX_USAGE_SELECTOR = 2 };

#define FX_IS_TEXTURE(t) ((t) >= FXT_TEXTURE && (t) <= FXT_TEXTURECUBE)
#define FX_IS_SAMPLER(t) ((t) >= FXT_SAMPLER && (t) <= FXT_SAMPLERCUBE)
#define FX_IS_SHADER(t)  ((t) == FXT_PIXELSHADER || (t) == FXT_VERTEXSHADER)

struct fx_state;
struct fx_expr;
struct fx_shinfo;

struct fx_param {
    const char *name, *semantic;             /* in the binary (NULL when empty) */
    uint32_t gname, gsemantic;               /* the same, as guest addresses (0 when empty) */
    uint32_t type, cls, rows, cols, elements, nmembers, flags, bytes;
    struct fx_param *members;                /* the elements, or else the structure's members */
    uint32_t nannos;
    struct fx_param *annos;
    uint8_t *data;                           /* bytes of value (NULL for samplers) */
    uint32_t object;                         /* a string, texture or shader leaf: its object id */
    uint32_t nstates;                        /* a sampler leaf: its states */
    struct fx_state *states;
    struct fx_param *top;                    /* the top-level parameter (or annotation) it is part of */
    uint64_t *serial;                        /* the top-level value's dirty serial (shared in a pool) */
    uint64_t own_serial;
    uint32_t array_lo, array_hi;             /* SetArrayRange */
    uint32_t handle;                         /* guest handle */
    uint8_t *owned;                          /* the value buffer this top-level parameter owns, or NULL */
    uint32_t rec, voff;                      /* byte offsets in the binary: a top-level parameter's or annotation's
                                                record (typedef, value, ...), and its value (plan 9's oracle) */
};

struct fx_state {
    uint32_t op, index;                      /* the state operation and its index (stage, light, ...) */
    struct fx_param param;                   /* its value's type and constant value */
    uint32_t usage;                          /* FX_USAGE_*: NONE for a constant */
    const uint8_t *blob;                     /* the resource: shader tokens, expression, or a name */
    uint32_t bloblen;
    struct fx_param *ref;                    /* FX_USAGE_PARAM: the parameter named; SELECTOR: the array */
    struct fx_expr *expr;                    /* an expression's (or a selector's index), parsed at first use */
    uint32_t at;                             /* its record's byte offset in the binary (operation, index, ...) */
};

struct fx_pass {
    const char *name;
    uint32_t gname, nannos, nstates, handle;
    struct fx_param *annos;
    struct fx_state *states;
};
struct fx_technique {
    const char *name;
    uint32_t gname, nannos, npasses, handle;
    struct fx_param *annos;
    struct fx_pass *passes;
};
struct fx_object {                           /* a string's text, a shader's tokens (in the binary) */
    const uint8_t *data;
    uint32_t size;
    uint32_t guest;                          /* the guest object made from it (a shader), or 0 */
    struct fx_shinfo *sh;                    /* a shader's constant table and preshader, read at first use */
};

/* parameter blocks (plan 10 Task 4, Ruling 10): a set made while a block records is one record, the
 * parameter and its whole value after the set, in call order */
struct fx_record {
    struct fx_param *p;
    uint8_t *data;                           /* p->bytes; holds a reference to each texture or shader it names */
};
struct fx_block {
    uint32_t handle;                         /* EndParameterBlock's D3DXHANDLE: a guest dword of its own */
    uint32_t n, cap;
    struct fx_record *recs;
    struct fx_block *next;
};

struct fx_pool;
struct fx_effect {
    uint32_t nparams, ntechs, nobjects;
    struct fx_param *params;
    struct fx_technique *techs;
    struct fx_object *objects;
    struct fx_pool *pool;                    /* NULL without a pool */
    /* guest memory: a copy of the binary (names and blobs point into it), and the handle records */
    uint32_t graw, glen, ghandles, nhandles;
    const uint8_t *raw;
    struct { uint8_t kind; void *p; } *hrec;
    uint32_t *strings, nstrings;             /* guest strings SetString made */
    /* techniques and passes (Task 6) */
    uint32_t device;                         /* the guest device passes are applied to */
    struct fx_technique *cur;                /* SetTechnique's (the first technique at first) */
    int begun;
    uint32_t begin_flags;
    struct fx_pass *pass;                    /* the pass BeginPass applied, or NULL */
    uint32_t saved;                          /* the state block Begin recorded, or 0 */
    uint64_t applied;                        /* g_fx_serial when the pass's values were last applied */
    struct fx_object *vs_obj, *ps_obj;       /* the shaders the pass set (their constants are the effect's) */
    /* parameter blocks (plan 10 Task 4) */
    struct fx_block *blocks;                 /* the ended blocks, newest first; they live until the effect does */
    struct fx_block *recording;              /* the block between BeginParameterBlock and End, or NULL */
};
enum { FXH_PARAM = 1, FXH_TECH, FXH_PASS };

/* fx_parse.c */
/* Parse len bytes of fx_2_0 binary into *out (zeroed by the caller): 0, or -1 with err set. */
int fx_parse(const uint8_t *data, uint32_t len, struct fx_effect *out, char *err, size_t errlen);
void fx_free(struct fx_effect *e);           /* host and guest memory (not the values' references) */
uint32_t fx_handle(const struct fx_param *p);
/* A parameter from a handle, or from a guest string naming one ("a", "s.member", "arr[2]"); NULL if none. */
struct fx_param *fx_param_of(struct fx_effect *e, uint32_t handle);
struct fx_technique *fx_tech_of(struct fx_effect *e, uint32_t handle);
struct fx_pass *fx_pass_of(struct fx_effect *e, uint32_t handle);
/* By name among the top level (parent NULL) or a parent's members/elements; NULL if none. */
struct fx_param *fx_find_param(struct fx_effect *e, struct fx_param *parent, const char *name);
/* Lay p's value out over base (members and elements after one another), with p as the top of its
 * members and serial as their dirty serial. */
void fx_bind(struct fx_param *p, uint8_t *base, uint64_t *serial);
/* Call fn for each leaf (no members, no elements) of p. */
void fx_leaves(struct fx_param *p, void (*fn)(struct fx_param *leaf, void *ctx), void *ctx);
/* A description of p's type (type, class, size, elements, members) for pool matching; malloc'd. */
char *fx_signature(const struct fx_param *p);
extern uint64_t g_fx_serial;                 /* bumped by every parameter change */

/* fx_effect.c */
struct fx_effect *fx_effect_of(uint32_t effect);   /* an ID3DXEffect's host structures */
/* Where a setter writes p's value: p->data, or while a block records a new record's bytes (a copy of p's
 * newest record in the block, else of its live value, with its own references). */
uint8_t *fx_dest(struct fx_effect *e, struct fx_param *p);

/* ---- fx_expr.c (Task 6): constant tables, and FXLC expressions (state values, array selectors and
 * shader preshaders) run on the CPU in float32 ---- */

enum { FX_RS_BOOL, FX_RS_INT4, FX_RS_FLOAT4, FX_RS_SAMPLER };      /* D3DXREGISTER_SET */
struct fx_ctab_entry {
    const char *name;
    struct fx_param *param;                  /* the effect parameter of that name, or NULL */
    uint32_t set, index, count;              /* register set, first register, registers */
    uint32_t type;                           /* its D3DXSHADER_TYPEINFO's offset in the table */
};
struct fx_ctab {
    const uint8_t *base;                     /* the table: the bytes after the CTAB fourcc */
    uint32_t size, n;
    struct fx_ctab_entry *e;
};
/* The comment of fourcc among those leading tokens (after tokens[0]): its data after the fourcc and its
 * dwords, or NULL. */
const uint32_t *fx_comment(const uint32_t *tokens, uint32_t ntokens, uint32_t fourcc, uint32_t *len);
/* The CTAB among the comments that follow tokens[0] (a version): 0 (n is 0 without one), or -1 with err. */
int fx_ctab_read(const uint32_t *tokens, uint32_t ntokens, struct fx_effect *e, struct fx_ctab *t, char *err,
                 size_t errlen);
void fx_ctab_free(struct fx_ctab *t);
/* Entry i's parameter value laid out as its registers (4 dwords each: floats for FLOAT4, ints for INT4, a
 * 0/1 in the first for BOOL), at most max of them; the registers written. */
uint32_t fx_ctab_regs(const struct fx_ctab *t, uint32_t i, uint32_t (*dst)[4], uint32_t max);

struct fx_expr {
    uint32_t version;                        /* 0x46580200 (a state's) or 0x46580201 (a preshader) */
    struct fx_ctab in;                       /* its inputs: parameters into the c table */
    const uint32_t *code;                    /* the FXLC instructions */
    uint32_t nins;
    float *lit;                              /* CLIT, as float32 */
    uint32_t nlit;
    uint32_t nin, ntemp, nout;               /* floats in the input, temporary and output tables */
    uint32_t *prsi, nranges;                 /* PRSI: (register, count) pairs of the outputs a preshader sets */
};
/* tokens: a 0x4658xxxx version, comments (CTAB, PRSI, CLIT, FXLC), the end token. 0, or -1 with err set;
 * an opcode the interpreter lacks names itself in err. */
int fx_expr_parse(const uint32_t *tokens, uint32_t ntokens, struct fx_effect *e, struct fx_expr *x, char *err,
                  size_t errlen);
void fx_expr_free(struct fx_expr *x);
/* Run x on its parameters' current values: the first nout floats of its output table into out. 0. */
int fx_expr_run(const struct fx_expr *x, const struct fx_effect *e, float *out, uint32_t nout);
uint64_t fx_expr_serial(const struct fx_expr *x);                  /* the newest serial among its inputs */
const char *fx_expr_opname(uint32_t opcode, uint32_t ninputs);     /* NULL when not implemented */
/* The FXLC opcodes (opcode << 16 | inputs) in tokens' expression; n set, the list malloc'd. */
uint32_t *fx_expr_ops(const uint32_t *tokens, uint32_t ntokens, uint32_t *n);

/* ---- fx_states.c (Task 6): the fx_2_0 state operations, and applying passes ---- */

enum { FXS_RENDER, FXS_STAGE, FXS_NPATCH, FXS_FVF, FXS_TRANSFORM, FXS_MATERIAL, FXS_LIGHT, FXS_LIGHTENABLE,
       FXS_VSHADER, FXS_PSHADER, FXS_CONST, FXS_TEXTURE, FXS_SAMPLER, FXS_SETSAMPLER };
enum { FXV_DWORD, FXV_FLOAT, FXV_COLOR, FXV_VEC3, FXV_VEC4, FXV_MATRIX, FXV_OBJECT, FXV_FLOATS, FXV_INTS, FXV_BOOLS };
struct fx_state_info {
    uint8_t cls;                             /* FXS_* */
    uint8_t vtype;                           /* FXV_*: the value it takes */
    uint16_t num;                            /* the D3D state, stage state, sampler state, transform, ... */
    const char *name;
};
const struct fx_state_info *fx_state(uint32_t op);     /* NULL: no such operation */

/* the constant table and preshader of one of an effect's shaders */
struct fx_shinfo {
    uint32_t version;
    struct fx_ctab ctab;
    struct fx_expr *pre;                     /* NULL without a preshader */
};
extern uint32_t g_fx_const_uploads;          /* Set*ShaderConstant* calls passes made (tests) */
/* technique and pass methods' work, on the effect's device */
int fx_tech_valid(struct fx_effect *e, struct fx_technique *t);
int fx_param_used(struct fx_effect *e, struct fx_param *p, struct fx_technique *t);
uint32_t fx_begin(struct fx_effect *e, uint32_t flags);            /* the state block Begin saves, or 0 */
void fx_apply_pass(struct fx_effect *e, struct fx_pass *ps, int all);   /* all 0: what changed only */
void fx_shader_free(struct fx_object *o);

#endif
