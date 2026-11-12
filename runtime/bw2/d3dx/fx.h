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
};

struct fx_state {
    uint32_t op, index;                      /* the state operation and its index (stage, light, ...) */
    struct fx_param param;                   /* its value's type and constant value */
    uint32_t usage;                          /* FX_USAGE_*: NONE for a constant */
    const uint8_t *blob;                     /* the resource: shader tokens, expression, or a name */
    uint32_t bloblen;
    struct fx_param *ref;                    /* FX_USAGE_PARAM: the parameter named */
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

#endif
