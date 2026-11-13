/*
 * fx_parse.c - the fx_2_0 effect binary into fx.h's host structures (plan 5
 * Task 5), its handles, and finding parameters by name.
 *
 * The binary (D3DX's compiled effect; tests/fxwrite.py writes the same):
 *   0xFEFF0901, then the body's offset; every offset counts from byte 8.
 *   body: parameters, techniques, (a count D3DX does not use), objects;
 *         each parameter: typedef, value, flags, annotation count, then the
 *         annotations as (typedef, value);
 *         each technique: name, annotation count, pass count, annotations,
 *         passes: name, annotation count, state count, annotations, states
 *         as (operation, index, typedef, value);
 *         object data: count, then (id, size, bytes padded to 4): strings'
 *         text and shader parameters' tokens;
 *         resources: count, then (technique or -1, pass or parameter,
 *         element or -1, state, usage, size, bytes): a state's shader or
 *         expression (usage 0), the parameter it names (1), an array
 *         selector (2).
 *   typedef: type, class, name, semantic, elements, then a vector's columns
 *         and rows, a scalar's or matrix's rows and columns, or a
 *         structure's member count and member typedefs (once for an array).
 *   value: numbers as dwords; an object id for a string, texture or shader;
 *         a sampler's state count and states.
 * A name is a dword size and that many bytes, its NUL included.
 *
 * The binary is copied into guest memory once: names, strings and shader
 * tokens point into that copy, so descriptions hand the guest their address.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "fx.h"

uint64_t g_fx_serial;

#define FX_MAGIC      0xFEFF0901u
#define MAX_ELEMENTS  65536u
#define MAX_NODES     (1u << 20)           /* parameters, members and elements in one effect */
#define MAX_DEPTH     32u

struct rd {
    const uint8_t *d;                      /* the data base: the binary + 8 */
    uint32_t n;                            /* bytes from there */
    struct fx_effect *e;
    char *err;
    size_t errlen;
    int bad;
    uint32_t depth, nodes;
};

static void fail(struct rd *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(struct rd *r, const char *fmt, ...) {
    if (r->bad) return;
    r->bad = 1;
    if (!r->err || !r->errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, r->errlen, fmt, ap);
    va_end(ap);
}
/* a dword at an offset from the data base (offsets in messages count from the binary's start) */
static uint32_t u32(struct rd *r, uint32_t off) {
    if (r->bad) return 0;
    if (off > r->n || r->n - off < 4) { fail(r, "read at 0x%X past the end (0x%X bytes)", off + 8u, r->n + 8u); return 0; }
    uint32_t v;
    memcpy(&v, r->d + off, 4);
    return v;
}
static void *alloc(struct rd *r, uint32_t n, size_t size) {
    if (r->bad) return NULL;
    void *p = calloc(n ? n : 1, size);
    if (!p) fail(r, "out of memory");
    return p;
}
/* a count of records of `each` bytes that must fit in what is left after off */
static int fits(struct rd *r, uint32_t count, uint32_t off, uint32_t each, const char *what) {
    if (r->bad) return 0;
    if (off > r->n || count > (r->n - off) / each) { fail(r, "%s count %u at 0x%X does not fit", what, count, off + 8u); return 0; }
    return 1;
}

static const char *name_at(struct rd *r, uint32_t off, uint32_t *guest) {
    *guest = 0;
    uint32_t n = u32(r, off);
    if (r->bad || !n) return NULL;
    if (n > r->n - off - 4) { fail(r, "name at 0x%X runs past the end", off + 8u); return NULL; }
    const char *s = (const char *)r->d + off + 4;
    if (s[n - 1]) { fail(r, "name at 0x%X is not terminated", off + 8u); return NULL; }
    *guest = hle_va(s);
    return s;
}

static int numeric_type(uint32_t t) { return t == FXT_BOOL || t == FXT_INT || t == FXT_FLOAT; }

static void typedef_at(struct rd *r, uint32_t *off, const struct fx_param *parent, struct fx_param *p, uint32_t flags) {
    if (r->bad) return;
    if (++r->nodes > MAX_NODES) { fail(r, "more than %u parameters", MAX_NODES); return; }
    if (r->depth >= MAX_DEPTH) { fail(r, "typedefs nested deeper than %u", MAX_DEPTH); return; }
    r->depth++;
    p->flags = flags;
    uint32_t at = *off + 8u;
    if (!parent) {
        p->type = u32(r, *off), p->cls = u32(r, *off + 4);
        p->name = name_at(r, u32(r, *off + 8), &p->gname);
        p->semantic = name_at(r, u32(r, *off + 12), &p->gsemantic);
        p->elements = u32(r, *off + 16);
        *off += 20;
        if (r->bad) goto out;
        if (p->elements > MAX_ELEMENTS) { fail(r, "typedef at 0x%X: %u elements", at, p->elements); goto out; }
        switch (p->cls) {
        case FXC_VECTOR:
            p->cols = u32(r, *off), p->rows = u32(r, *off + 4), *off += 8;
            if (p->rows != 1 || p->cols < 1 || p->cols > 4 || !numeric_type(p->type))
                fail(r, "typedef at 0x%X: vector type %u, %u x %u", at, p->type, p->rows, p->cols);
            p->bytes = 4 * p->rows * p->cols;
            break;
        case FXC_SCALAR: case FXC_MATRIX_ROWS: case FXC_MATRIX_COLUMNS:
            p->rows = u32(r, *off), p->cols = u32(r, *off + 4), *off += 8;
            if (p->rows < 1 || p->rows > 4 || p->cols < 1 || p->cols > 4 || !numeric_type(p->type)
                || (p->cls == FXC_SCALAR && (p->rows != 1 || p->cols != 1)))
                fail(r, "typedef at 0x%X: class %u type %u, %u x %u", at, p->cls, p->type, p->rows, p->cols);
            p->bytes = 4 * p->rows * p->cols;
            break;
        case FXC_STRUCT:
            p->nmembers = u32(r, *off), *off += 4;
            if (!r->bad && (!p->nmembers || p->nmembers > r->n / 20))
                fail(r, "typedef at 0x%X: %u structure members", at, p->nmembers);
            break;
        case FXC_OBJECT:
            if (p->type < FXT_STRING || p->type > FXT_VERTEXSHADER)
                fail(r, "typedef at 0x%X: object type %u", at, p->type);
            p->bytes = FX_IS_SAMPLER(p->type) ? 0 : 4;
            break;
        default:
            fail(r, "typedef at 0x%X: class %u", at, p->cls);
        }
    } else {                                                  /* an array element */
        p->type = parent->type, p->cls = parent->cls, p->name = parent->name, p->semantic = parent->semantic;
        p->gname = parent->gname, p->gsemantic = parent->gsemantic, p->rows = parent->rows, p->cols = parent->cols;
        p->nmembers = parent->nmembers, p->bytes = parent->cls == FXC_STRUCT ? 0 : parent->bytes;
    }
    if (r->bad) goto out;
    if (p->elements) {
        p->members = alloc(r, p->elements, sizeof *p->members);
        uint32_t save = *off, total = 0;
        for (uint32_t i = 0; i < p->elements && !r->bad; i++) {
            *off = save;
            typedef_at(r, off, p, &p->members[i], flags);
            total += p->members[i].bytes;
        }
        p->bytes = total;
    } else if (p->cls == FXC_STRUCT) {
        uint32_t n = p->nmembers;
        p->nmembers = 0;
        p->members = alloc(r, n, sizeof *p->members);
        for (uint32_t i = 0; i < n && !r->bad; i++) {
            p->nmembers = i + 1;
            typedef_at(r, off, NULL, &p->members[i], flags);
            p->bytes += p->members[i].bytes;
        }
    }
out:
    r->depth--;
}

static void state_at(struct rd *r, uint32_t off, struct fx_state *st);
static uint32_t children(const struct fx_param *p) { return p->elements ? p->elements : p->cls == FXC_STRUCT ? p->nmembers : 0; }

static void value_at(struct rd *r, struct fx_param *p, uint32_t *off) {
    if (r->bad) return;
    p->voff = *off + 8u;
    uint32_t n = children(p);
    if (n) {
        for (uint32_t i = 0; i < n && !r->bad; i++) value_at(r, &p->members[i], off);
        return;
    }
    if (p->cls != FXC_OBJECT) {
        if (*off > r->n || p->bytes > r->n - *off) { fail(r, "value at 0x%X runs past the end", *off + 8u); return; }
        memcpy(p->data, r->d + *off, p->bytes);
        *off += p->bytes;
    } else if (FX_IS_SAMPLER(p->type)) {
        uint32_t count = u32(r, *off);
        *off += 4;
        if (!fits(r, count, *off, 16, "sampler state")) return;
        p->states = alloc(r, count, sizeof *p->states);
        for (uint32_t i = 0; i < count && !r->bad; i++, *off += 16) {
            p->nstates = i + 1;
            state_at(r, *off, &p->states[i]);
        }
    } else {
        p->object = u32(r, *off);
        if (!r->bad && p->object >= r->e->nobjects)
            fail(r, "value at 0x%X: object %u of %u", *off + 8u, p->object, r->e->nobjects);
        *off += 4;
    }
}

static void bind(struct fx_param *p, struct fx_param *top, uint8_t *base, uint64_t *serial) {
    p->data = p->bytes ? base : NULL, p->serial = serial, p->top = top;
    uint32_t off = 0;
    for (uint32_t i = 0; i < children(p); i++) {
        bind(&p->members[i], top, base ? base + off : NULL, serial);
        off += p->members[i].bytes;
    }
}
void fx_bind(struct fx_param *p, uint8_t *base, uint64_t *serial) { bind(p, p, base, serial); }
/* a top-level parameter, annotation or state value: its own value buffer */
static void top_init(struct rd *r, struct fx_param *p) {
    if (r->bad) return;
    p->owned = alloc(r, p->bytes, 1);
    if (!p->owned) return;
    fx_bind(p, p->owned, &p->own_serial);
}

static void state_at(struct rd *r, uint32_t off, struct fx_state *st) {
    st->op = u32(r, off), st->index = u32(r, off + 4), st->usage = FX_USAGE_NONE;
    st->at = off + 8u;
    uint32_t toff = u32(r, off + 8), voff = u32(r, off + 12);
    typedef_at(r, &toff, NULL, &st->param, 0);
    top_init(r, &st->param);
    value_at(r, &st->param, &voff);
}
static void anno_at(struct rd *r, uint32_t off, struct fx_param *a) {
    uint32_t toff = u32(r, off), voff = u32(r, off + 4);
    typedef_at(r, &toff, NULL, a, FX_ANNOTATION);
    a->rec = off + 8u;
    top_init(r, a);
    value_at(r, a, &voff);
}
static void annos_at(struct rd *r, uint32_t *o, uint32_t n, struct fx_param **out, uint32_t *count) {
    *count = 0;
    if (!fits(r, n, *o, 8, "annotation")) return;
    *out = alloc(r, n, sizeof **out);
    for (uint32_t i = 0; i < n && !r->bad; i++, *o += 8) {
        *count = i + 1;
        anno_at(r, *o, &(*out)[i]);
    }
}

/* ---- walking everything ---- */

void fx_leaves(struct fx_param *p, void (*fn)(struct fx_param *leaf, void *ctx), void *ctx) {
    uint32_t n = children(p);
    if (!n) { fn(p, ctx); return; }
    for (uint32_t i = 0; i < n; i++) fx_leaves(&p->members[i], fn, ctx);
}
/* every state of the effect: passes', then samplers' (in parameters, members and annotations) */
static void sampler_states(struct fx_param *leaf, void *ctx) {
    void (**fn)(struct fx_state *) = ctx;
    for (uint32_t i = 0; i < leaf->nstates; i++) (*fn)(&leaf->states[i]);
}
static void each_state(struct fx_effect *e, void (*fn)(struct fx_state *)) {
    for (uint32_t t = 0; t < e->ntechs; t++)
        for (uint32_t p = 0; p < e->techs[t].npasses; p++)
            for (uint32_t s = 0; s < e->techs[t].passes[p].nstates; s++) fn(&e->techs[t].passes[p].states[s]);
    for (uint32_t i = 0; i < e->nparams; i++) fx_leaves(&e->params[i], sampler_states, &fn);
}

static void free_param(struct fx_param *p);
static void free_states(struct fx_state *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        free_param(&s[i].param);
        if (s[i].expr) fx_expr_free(s[i].expr), free(s[i].expr);
    }
    free(s);
}
static void free_annos(struct fx_param *a, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) free_param(&a[i]);
    free(a);
}
static void free_param(struct fx_param *p) {
    uint32_t n = children(p);
    if (p->members) for (uint32_t i = 0; i < n; i++) free_param(&p->members[i]);
    free(p->members);
    free_states(p->states, p->nstates);
    free_annos(p->annos, p->nannos);
    free(p->owned);
}
void fx_free(struct fx_effect *e) {
    for (uint32_t i = 0; i < e->nparams; i++) free_param(&e->params[i]);
    free(e->params);
    for (uint32_t t = 0; t < e->ntechs; t++) {
        struct fx_technique *tq = &e->techs[t];
        free_annos(tq->annos, tq->nannos);
        for (uint32_t p = 0; p < tq->npasses; p++) {
            free_annos(tq->passes[p].annos, tq->passes[p].nannos);
            free_states(tq->passes[p].states, tq->passes[p].nstates);
        }
        free(tq->passes);
    }
    free(e->techs);
    for (uint32_t i = 0; i < e->nobjects; i++) fx_shader_free(&e->objects[i]);
    free(e->objects);
    free(e->hrec);
    for (uint32_t i = 0; i < e->nstrings; i++) hle_free(hle_ptr(e->strings[i]));
    free(e->strings);
    if (e->graw) hle_free(hle_ptr(e->graw));
    if (e->ghandles) hle_free(hle_ptr(e->ghandles));
    memset(e, 0, sizeof *e);
}

/* ---- names ---- */

struct fx_param *fx_find_param(struct fx_effect *e, struct fx_param *parent, const char *name) {
    if (!name || !*name) return NULL;
    size_t len = strcspn(name, ".[@");
    struct fx_param *list = parent ? parent->members : e->params;
    uint32_t n = parent ? (parent->elements || parent->cls != FXC_STRUCT ? 0 : parent->nmembers) : e->nparams;
    for (uint32_t i = 0; i < n; i++) {
        struct fx_param *p = &list[i];
        if (!p->name || strlen(p->name) != len || strncmp(p->name, name, len)) continue;
        const char *rest = name + len;
        if (!*rest) return p;
        if (*rest == '.') return fx_find_param(e, p, rest + 1);
        if (*rest == '@') {
            for (uint32_t k = 0; k < p->nannos; k++)
                if (p->annos[k].name && !strcmp(p->annos[k].name, rest + 1)) return &p->annos[k];
            return NULL;
        }
        char *end;                                            /* '[' */
        unsigned long idx = strtoul(rest + 1, &end, 10);
        if (end == rest + 1 || *end != ']' || !p->elements || idx >= p->elements) return NULL;
        struct fx_param *el = &p->members[idx];
        if (!end[1]) return el;
        return end[1] == '.' ? fx_find_param(e, el, end + 2) : NULL;
    }
    return NULL;
}

static void *handle_rec(struct fx_effect *e, uint32_t h, int kind) {
    if (!e->ghandles || h < e->ghandles || h >= e->ghandles + 4u * e->nhandles || (h - e->ghandles) & 3) return NULL;
    uint32_t i = (h - e->ghandles) / 4;
    return e->hrec[i].kind == kind ? e->hrec[i].p : NULL;
}
static int is_handle(struct fx_effect *e, uint32_t h) {
    return e->ghandles && h >= e->ghandles && h < e->ghandles + 4u * e->nhandles;
}
uint32_t fx_handle(const struct fx_param *p) { return p ? p->handle : 0; }
struct fx_param *fx_param_of(struct fx_effect *e, uint32_t h) {
    if (!h) return NULL;
    if (is_handle(e, h)) return handle_rec(e, h, FXH_PARAM);
    return fx_find_param(e, NULL, (const char *)hle_ptr(h));
}
struct fx_technique *fx_tech_of(struct fx_effect *e, uint32_t h) {
    if (!h) return NULL;
    if (is_handle(e, h)) return handle_rec(e, h, FXH_TECH);
    const char *s = hle_ptr(h);
    for (uint32_t i = 0; i < e->ntechs; i++)
        if (e->techs[i].name && !strcmp(e->techs[i].name, s)) return &e->techs[i];
    return NULL;
}
struct fx_pass *fx_pass_of(struct fx_effect *e, uint32_t h) { return h ? handle_rec(e, h, FXH_PASS) : NULL; }

/* ---- handles: one record per parameter node, annotation, technique and pass ---- */

static uint32_t count_param(const struct fx_param *p, int annos) {
    uint32_t n = 1;
    for (uint32_t i = 0; i < children(p); i++) n += count_param(&p->members[i], 0);
    if (annos) for (uint32_t i = 0; i < p->nannos; i++) n += count_param(&p->annos[i], 0);
    return n;
}
static void give(struct fx_effect *e, uint32_t *next, int kind, void *p, uint32_t *handle) {
    uint32_t i = (*next)++;
    e->hrec[i].kind = (uint8_t)kind, e->hrec[i].p = p;
    *handle = e->ghandles + 4 * i;
    MEM32(*handle) = i;
}
static void give_param(struct fx_effect *e, uint32_t *next, struct fx_param *p, int annos) {
    give(e, next, FXH_PARAM, p, &p->handle);
    for (uint32_t i = 0; i < children(p); i++) give_param(e, next, &p->members[i], 0);
    if (annos) for (uint32_t i = 0; i < p->nannos; i++) give_param(e, next, &p->annos[i], 0);
}
static int make_handles(struct rd *r) {
    struct fx_effect *e = r->e;
    uint32_t n = 0;
    for (uint32_t i = 0; i < e->nparams; i++) n += count_param(&e->params[i], 1);
    for (uint32_t t = 0; t < e->ntechs; t++) {
        n += 1;
        for (uint32_t a = 0; a < e->techs[t].nannos; a++) n += count_param(&e->techs[t].annos[a], 0);
        for (uint32_t p = 0; p < e->techs[t].npasses; p++) {
            n += 1;
            for (uint32_t a = 0; a < e->techs[t].passes[p].nannos; a++) n += count_param(&e->techs[t].passes[p].annos[a], 0);
        }
    }
    e->hrec = alloc(r, n, sizeof *e->hrec);
    void *g = r->bad ? NULL : hle_alloc(4 * (n ? n : 1));
    if (!r->bad && !g) fail(r, "out of guest memory");
    if (r->bad) return -1;
    e->ghandles = hle_va(g), e->nhandles = n;
    uint32_t next = 0;
    for (uint32_t i = 0; i < e->nparams; i++) give_param(e, &next, &e->params[i], 1);
    for (uint32_t t = 0; t < e->ntechs; t++) {
        struct fx_technique *tq = &e->techs[t];
        give(e, &next, FXH_TECH, tq, &tq->handle);
        for (uint32_t a = 0; a < tq->nannos; a++) give_param(e, &next, &tq->annos[a], 0);
        for (uint32_t p = 0; p < tq->npasses; p++) {
            give(e, &next, FXH_PASS, &tq->passes[p], &tq->passes[p].handle);
            for (uint32_t a = 0; a < tq->passes[p].nannos; a++) give_param(e, &next, &tq->passes[p].annos[a], 0);
        }
    }
    return 0;
}

/* a string leaf's value: the guest address of its text (in the guest copy), or 0 */
static struct fx_effect *g_strings_of;
static void string_value(struct fx_param *leaf, void *ctx) {
    (void)ctx;
    if (leaf->cls != FXC_OBJECT || leaf->type != FXT_STRING) return;
    const struct fx_object *o = &g_strings_of->objects[leaf->object];
    uint32_t v = o->size && !o->data[o->size - 1] ? hle_va(o->data) : 0;
    memcpy(leaf->data, &v, 4);
}
static void string_annos(struct fx_param *a, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) fx_leaves(&a[i], string_value, NULL);
}
static void string_state(struct fx_state *s) { fx_leaves(&s->param, string_value, NULL); }

/* a usage-1 resource names a parameter */
static struct rd *g_refs_rd;
static void resolve_ref(struct fx_state *s) {
    if (s->usage != FX_USAGE_PARAM || g_refs_rd->bad) return;
    if (!s->bloblen || s->blob[s->bloblen - 1]) { fail(g_refs_rd, "state %u: the parameter name is not terminated", s->op); return; }
    s->ref = fx_find_param(g_refs_rd->e, NULL, (const char *)s->blob);
    if (!s->ref) fail(g_refs_rd, "state %u names parameter '%s', which the effect lacks", s->op, (const char *)s->blob);
}

static struct fx_state *resource_state(struct rd *r, uint32_t ti, uint32_t idx, uint32_t ei, uint32_t si) {
    struct fx_effect *e = r->e;
    if (ti == 0xFFFFFFFFu) {
        if (idx >= e->nparams) { fail(r, "resource for parameter %u of %u", idx, e->nparams); return NULL; }
        struct fx_param *p = &e->params[idx];
        if (ei != 0xFFFFFFFFu && p->elements) {
            if (ei >= p->elements) { fail(r, "resource for element %u of %u", ei, p->elements); return NULL; }
            p = &p->members[ei];
        }
        if (p->cls != FXC_OBJECT || !FX_IS_SAMPLER(p->type) || si >= p->nstates) {
            fail(r, "resource for state %u of parameter %u, which has %u", si, idx, p->nstates);
            return NULL;
        }
        return &p->states[si];
    }
    if (ti >= e->ntechs || idx >= e->techs[ti].npasses || si >= e->techs[ti].passes[idx].nstates) {
        fail(r, "resource for technique %u pass %u state %u", ti, idx, si);
        return NULL;
    }
    return &e->techs[ti].passes[idx].states[si];
}

int fx_parse(const uint8_t *data, uint32_t len, struct fx_effect *e, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    struct rd r = { .e = e, .err = err, .errlen = errlen };
    uint32_t magic = 0;
    if (len >= 4) memcpy(&magic, data, 4);
    if (len < 8 || magic != FX_MAGIC) { fail(&r, "not an fx_2_0 effect (0x%08X, %u bytes)", magic, len); return -1; }
    void *g = hle_alloc(len);
    if (!g) { fail(&r, "out of guest memory"); return -1; }
    memcpy(g, data, len);
    e->graw = hle_va(g), e->glen = len, e->raw = g;
    r.d = e->raw + 8, r.n = len - 8;

    uint32_t o;
    memcpy(&o, e->raw + 4, 4);
    uint32_t np = u32(&r, o), nt = u32(&r, o + 4), nobj = u32(&r, o + 12);
    o += 16;
    if (!r.bad && nobj > r.n) fail(&r, "%u objects", nobj);
    if (!fits(&r, np, o, 16, "parameter") || !fits(&r, nt, o, 12, "technique")) return -1;
    e->objects = alloc(&r, nobj, sizeof *e->objects);
    e->nobjects = nobj;
    e->params = alloc(&r, np, sizeof *e->params);
    for (uint32_t i = 0; i < np && !r.bad; i++) {
        struct fx_param *p = &e->params[i];
        e->nparams = i + 1;
        uint32_t toff = u32(&r, o), voff = u32(&r, o + 4), flags = u32(&r, o + 8), na = u32(&r, o + 12);
        o += 16;
        typedef_at(&r, &toff, NULL, p, flags);
        p->rec = o - 16u + 8u;                                /* the 16-byte record just read, from byte 0 */
        top_init(&r, p);
        value_at(&r, p, &voff);
        annos_at(&r, &o, na, &p->annos, &p->nannos);
    }
    e->techs = alloc(&r, nt, sizeof *e->techs);
    for (uint32_t t = 0; t < nt && !r.bad; t++) {
        struct fx_technique *tq = &e->techs[t];
        e->ntechs = t + 1;
        tq->name = name_at(&r, u32(&r, o), &tq->gname);
        uint32_t na = u32(&r, o + 4), npass = u32(&r, o + 8);
        o += 12;
        annos_at(&r, &o, na, &tq->annos, &tq->nannos);
        if (!fits(&r, npass, o, 12, "pass")) break;
        tq->passes = alloc(&r, npass, sizeof *tq->passes);
        for (uint32_t p = 0; p < npass && !r.bad; p++) {
            struct fx_pass *ps = &tq->passes[p];
            tq->npasses = p + 1;
            ps->name = name_at(&r, u32(&r, o), &ps->gname);
            uint32_t pa = u32(&r, o + 4), ns = u32(&r, o + 8);
            o += 12;
            annos_at(&r, &o, pa, &ps->annos, &ps->nannos);
            if (!fits(&r, ns, o, 16, "state")) break;
            ps->states = alloc(&r, ns, sizeof *ps->states);
            for (uint32_t s = 0; s < ns && !r.bad; s++, o += 16) {
                ps->nstates = s + 1;
                state_at(&r, o, &ps->states[s]);
            }
        }
    }
    uint32_t nstr = u32(&r, o), nres = u32(&r, o + 4);
    o += 8;
    if (!fits(&r, nstr, o, 8, "object data") || !fits(&r, nres, o, 24, "resource")) return -1;
    for (uint32_t i = 0; i < nstr && !r.bad; i++) {
        uint32_t id = u32(&r, o), size = u32(&r, o + 4);
        o += 8;
        if (r.bad) break;
        if (id >= nobj) { fail(&r, "object data for object %u of %u", id, nobj); break; }
        if (size > r.n - o) { fail(&r, "object %u's %u bytes run past the end", id, size); break; }
        e->objects[id].data = r.d + o, e->objects[id].size = size;
        o += size + (-size & 3u);
    }
    for (uint32_t i = 0; i < nres && !r.bad; i++) {
        uint32_t ti = u32(&r, o), idx = u32(&r, o + 4), ei = u32(&r, o + 8), si = u32(&r, o + 12);
        uint32_t usage = u32(&r, o + 16), size = u32(&r, o + 20);
        o += 24;
        if (r.bad) break;
        if (size > r.n - o) { fail(&r, "resource %u's %u bytes run past the end", i, size); break; }
        struct fx_state *st = resource_state(&r, ti, idx, ei, si);
        if (!st) break;
        if (usage > FX_USAGE_SELECTOR) { fail(&r, "resource %u: usage %u", i, usage); break; }
        st->usage = usage, st->blob = r.d + o, st->bloblen = size;
        if (usage == FX_USAGE_BLOB && st->param.cls == FXC_OBJECT && FX_IS_SHADER(st->param.type) && !st->param.elements)
            e->objects[st->param.object].data = st->blob, e->objects[st->param.object].size = size;
        o += size + (-size & 3u);
    }
    if (r.bad) return -1;
    g_refs_rd = &r;
    each_state(e, resolve_ref);
    if (r.bad) return -1;
    g_strings_of = e;
    for (uint32_t i = 0; i < e->nparams; i++) fx_leaves(&e->params[i], string_value, NULL), string_annos(e->params[i].annos, e->params[i].nannos);
    for (uint32_t t = 0; t < e->ntechs; t++) {
        string_annos(e->techs[t].annos, e->techs[t].nannos);
        for (uint32_t p = 0; p < e->techs[t].npasses; p++) string_annos(e->techs[t].passes[p].annos, e->techs[t].passes[p].nannos);
    }
    each_state(e, string_state);
    return make_handles(&r);
}

/* ---- pool signatures ---- */

static void sig_cat(char **s, size_t *n, size_t *cap, const struct fx_param *p) {
    char b[96];
    int k = snprintf(b, sizeof b, "%s:%u.%u.%u.%u.%u(", p->name ? p->name : "", p->type, p->cls, p->rows, p->cols, p->elements);
    if (*n + (size_t)k + 2 > *cap) *s = realloc(*s, *cap = 2 * (*cap + (size_t)k + 2));
    memcpy(*s + *n, b, (size_t)k + 1), *n += (size_t)k;
    if (p->cls == FXC_STRUCT) {
        const struct fx_param *m = p->elements ? &p->members[0] : p;
        for (uint32_t i = 0; i < m->nmembers && m->members; i++) sig_cat(s, n, cap, &m->members[i]);
    }
    if (*n + 2 > *cap) *s = realloc(*s, *cap = 2 * *cap + 2);
    (*s)[(*n)++] = ')', (*s)[*n] = 0;
}
char *fx_signature(const struct fx_param *p) {
    size_t n = 0, cap = 64;
    char *s = malloc(cap);
    s[0] = 0;
    sig_cat(&s, &n, &cap, p);
    return s;
}
