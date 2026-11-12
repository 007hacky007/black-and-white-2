/*
 * shims_crt.c - msvcrt for the lifted D3DX (plan 9 Task 3, Ruling 7).
 *
 * The disc's d3dx9_25.dll links msvcrt.dll dynamically; the game itself does
 * not. These answer the names the D3DX entry points reach, in the "C" locale,
 * as the C and msvcrt documentation describe them. Every msvcrt function is
 * cdecl: the caller pops the arguments, so each shim pops only its return
 * address. Results that are doubles go on the lifted FPU stack (g_st), as an
 * x87 function returns them in st(0). Every other msvcrt import stays a stub
 * that stops loudly by name when called (hle_resolve_or_stub).
 *
 * Each answer is checked in shims_selftest.c first.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>
#include "win32hle.h"
#include "bw2.h"

#define CRET(v) do { g_eax = (uint32_t)(v); g_esp += 4; return; } while (0)
#define X87_PUSH(v) fp_push_impl(g_st, &g_fp_top, (v))
#define X87_POP()   fp_pop_impl(g_st, &g_fp_top)

/* A stop for a path the port does not take, naming the caller. */
static void crt_die(const char *what) {
    fprintf(stderr, "[bw2] FATAL: msvcrt: %s (from guest 0x%08X, sub_%08X)\n", what, MEM32(g_esp), g_cur_func);
    fflush(stderr);
    abort();
}

/* Argument dwords i and i+1 as the double a cdecl call pushed there. */
static double arg_double(int i) {
    uint64_t u = (uint64_t)A32(i) | (uint64_t)A32(i + 1) << 32;
    double d;
    memcpy(&d, &u, 8);
    return d;
}

/* ---- memory: the guest heap (zeroed blocks: a run does not depend on what a
 * block held before), 16-byte aligned. A failed allocation is 0, as msvcrt's.
 * A pointer this heap did not hand out stops loudly. */
static void *heap_block(uint32_t va, const char *fn) {
    void *p = hle_ptr(va);
    if (p && !hle_heap_ok(p)) {
        char what[96];
        snprintf(what, sizeof what, "%s of 0x%08X, which is not a live heap block", fn, va);
        crt_die(what);
    }
    return p;
}
static void c_malloc(void) { CRET(hle_va(hle_heap_alloc(A32(0), 1))); }
static void c_calloc(void) {
    uint64_t n = (uint64_t)A32(0) * A32(1);
    CRET(n > 0xFFFFFFFFull ? 0 : hle_va(hle_heap_alloc((uint32_t)n, 1)));
}
static void c_free(void) {
    void *p = heap_block(A32(0), "free");
    if (p) hle_heap_free(p);
    CRET(0);
}
static void c_realloc(void) {
    void *p = heap_block(A32(0), "realloc");
    uint32_t n = A32(1);
    if (p && !n) { hle_heap_free(p); CRET(0); }              /* realloc(p, 0) frees p */
    CRET(hle_va(hle_heap_realloc(p, n, 1)));                  /* realloc(NULL, n) is malloc(n) */
}
static void c_strdup(void) {
    const char *s = ASTR(0);
    if (!s) CRET(0);
    size_t n = strlen(s) + 1;
    char *d = hle_heap_alloc((uint32_t)n, 0);
    if (d) memcpy(d, s, n);
    CRET(hle_va(d));
}

/* ---- strings and characters: the "C" locale, ASCII only ---- */
static int c_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static void c_strchr(void) {
    const char *s = ASTR(0);
    char ch = (char)A32(1);
    for (;; s++) {
        if (*s == ch) CRET(hle_va(s));
        if (!*s) CRET(0);
    }
}
static void c_stricmp(void) {
    const uint8_t *a = (const uint8_t *)ASTR(0), *b = (const uint8_t *)ASTR(1);
    int d;
    while ((d = c_lower(*a) - c_lower(*b)) == 0 && *a) a++, b++;
    CRET((int32_t)d);
}
static void c_memmove(void) {
    if (A32(2)) memmove(APTR(0), APTR(1), A32(2));
    CRET(A32(0));
}
static void c_wcslen(void) {
    uint32_t s = A32(0), n = 0;
    while (MEM16(s + 2u * n)) n++;
    CRET(n);
}
/* The "C" locale's classes; bytes 0x80-0xFF and anything outside 0..255 (EOF) have none. */
enum { UP = 1, LO = 2, DI = 4, SP = 8, HX = 16 };
static int c_class(uint32_t c) {
    if (c > 0x7F) return 0;
    if (c >= 'A' && c <= 'Z') return UP | (c <= 'F' ? HX : 0);
    if (c >= 'a' && c <= 'z') return LO | (c <= 'f' ? HX : 0);
    if (c >= '0' && c <= '9') return DI | HX;
    if (c == ' ' || (c >= 9 && c <= 13)) return SP;
    return 0;
}
/* msvcrt's ctype answers are the class bits, not 1: only zero or not is promised. */
static void c_isalpha(void)  { CRET((c_class(A32(0)) & (UP | LO)) != 0); }
static void c_isalnum(void)  { CRET((c_class(A32(0)) & (UP | LO | DI)) != 0); }
static void c_isdigit(void)  { CRET((c_class(A32(0)) & DI) != 0); }
static void c_isxdigit(void) { CRET((c_class(A32(0)) & HX) != 0); }
static void c_isspace(void)  { CRET((c_class(A32(0)) & SP) != 0); }
static void c_tolower(void)  { uint32_t c = A32(0); CRET(c >= 'A' && c <= 'Z' ? c + 32 : c); }
static void c_toupper(void)  { uint32_t c = A32(0); CRET(c >= 'a' && c <= 'z' ? c - 32 : c); }

/* ---- numbers ---- */
/* atoi/atol: white space, a sign, decimal digits; an overflow wraps (32-bit). */
static void c_atoi(void) {
    const char *s = ASTR(0);
    while (c_class((uint8_t)*s) & SP) s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    CRET(neg ? 0u - v : v);
}
/* atof: msvcrt's grammar (white space, a sign, digits with one '.', an
 * exponent led by e, E, d or D; no hex, inf or nan), read in the "C" locale. */
static locale_t c_locale(void) {
    static locale_t loc;
    if (!loc) loc = newlocale(LC_ALL_MASK, "C", NULL);
    if (!loc) hle_fatal("msvcrt: no \"C\" locale for atof");
    return loc;
}
static void c_atof(void) {
    const char *s = ASTR(0);
    while (c_class((uint8_t)*s) & SP) s++;
    char buf[512];
    size_t k = 0, digits = 0;
    if (*s == '-' || *s == '+') buf[k++] = *s++;
    while (k < sizeof buf - 8 && *s >= '0' && *s <= '9') buf[k++] = *s++, digits++;
    if (*s == '.' && k < sizeof buf - 8) {
        buf[k++] = *s++;
        while (k < sizeof buf - 8 && *s >= '0' && *s <= '9') buf[k++] = *s++, digits++;
    }
    if (digits && (*s == 'e' || *s == 'E' || *s == 'd' || *s == 'D')) {
        const char *e = s + 1;
        char eb[8];
        size_t ek = 0;
        if (*e == '-' || *e == '+') eb[ek++] = *e++;
        if (*e >= '0' && *e <= '9') {
            buf[k++] = 'e';
            memcpy(buf + k, eb, ek), k += ek;
            while (k < sizeof buf - 1 && *e >= '0' && *e <= '9') buf[k++] = *e++;
        }
    }
    if (*s && k >= sizeof buf - 8) crt_die("atof of a number longer than 500 characters");
    buf[k] = 0;
    X87_PUSH(digits ? strtod_l(buf, NULL, c_locale()) : 0.0);
    CRET(0);
}
static void c_floor(void) { X87_PUSH(floor(arg_double(0))); CRET(0); }
static void c_ceil(void)  { X87_PUSH(ceil(arg_double(0))); CRET(0); }
static void c_ldexp(void) { X87_PUSH(ldexp(arg_double(0), (int32_t)A32(2))); CRET(0); }
static void c_frexp(void) {
    int e = 0;
    double m = frexp(arg_double(0), &e);
    if (A32(2)) MEM32(A32(2)) = (uint32_t)e;
    X87_PUSH(m);
    CRET(0);
}
static void c_modf(void) {
    double ip = 0, f = modf(arg_double(0), &ip);
    uint32_t p = A32(2);
    if (p) { uint64_t u; memcpy(&u, &ip, 8); MEM32(p) = (uint32_t)u; MEM32(p + 4) = (uint32_t)(u >> 32); }
    X87_PUSH(f);
    CRET(0);
}
static void c_finite(void) { CRET(isfinite(arg_double(0)) != 0); }
static void c_isnan(void)  { CRET(isnan(arg_double(0)) != 0); }
/* _fpclass: msvcrt's _FPCLASS_* values. */
static void c_fpclass(void) {
    double d = arg_double(0);
    uint64_t u;
    memcpy(&u, &d, 8);
    int neg = (int)(u >> 63);
    uint32_t r;
    if (isnan(d)) r = (u >> 51 & 1) ? 0x2 /* QNAN */ : 0x1 /* SNAN */;
    else if (isinf(d)) r = neg ? 0x4 : 0x200;
    else if (d == 0) r = neg ? 0x20 : 0x40;
    else if (fpclassify(d) == FP_SUBNORMAL) r = neg ? 0x10 : 0x80;
    else r = neg ? 0x8 : 0x100;
    CRET(r);
}

/* _controlfp(new, mask): _controlfp's encoding to and from the x87 control
 * word. Exception masks: _EM_INEXACT 0x1 (PM, bit 5), _EM_UNDERFLOW 0x2 (UM,
 * bit 4), _EM_OVERFLOW 0x4 (OM, bit 3), _EM_ZERODIVIDE 0x8 (ZM, bit 2),
 * _EM_INVALID 0x10 (IM, bit 0), _EM_DENORMAL 0x80000 (DM, bit 1); rounding
 * _MCW_RC 0x300 (RC, bits 10-11, the same order); precision _PC_24 0x20000,
 * _PC_53 0x10000, _PC_64 0 (PC, bits 8-9: 00, 10, 11); _IC_AFFINE 0x40000
 * (IC, bit 12). As documented, _controlfp leaves the denormal mask as it is
 * (that is _control87's). The SSE control register is not changed. */
static const struct { uint32_t cfp; uint16_t x87; } g_cw_bits[] = {
    { 0x00001u, 1u << 5 }, { 0x00002u, 1u << 4 }, { 0x00004u, 1u << 3 }, { 0x00008u, 1u << 2 },
    { 0x00010u, 1u << 0 }, { 0x80000u, 1u << 1 }, { 0x00100u, 1u << 10 }, { 0x00200u, 1u << 11 },
    { 0x40000u, 1u << 12 } };
#define CW_X87_MASK 0x1F3Fu
static uint32_t cw_to_cfp(uint16_t cw) {
    uint32_t r = 0;
    for (size_t i = 0; i < sizeof g_cw_bits / sizeof *g_cw_bits; i++)
        if (cw & g_cw_bits[i].x87) r |= g_cw_bits[i].cfp;
    static const uint32_t pc[4] = { 0x20000u, 0x30000u /* reserved */, 0x10000u, 0 };
    return r | pc[(cw >> 8) & 3];
}
static uint16_t cfp_to_cw(uint32_t c) {
    uint16_t r = 0;
    for (size_t i = 0; i < sizeof g_cw_bits / sizeof *g_cw_bits; i++)
        if (c & g_cw_bits[i].cfp) r |= g_cw_bits[i].x87;
    static const uint16_t pc[4] = { 3u << 8 /* _PC_64 */, 2u << 8 /* _PC_53 */, 0 /* _PC_24 */, 0 };
    return (uint16_t)(r | pc[(c >> 16) & 3]);
}
static void c_controlfp(void) {
    uint32_t mask = A32(1) & ~0x80000u & 0x0007031Fu;
    if (mask) {
        uint32_t c = (cw_to_cfp(g_fpu_cw) & ~mask) | (A32(0) & mask);
        g_fpu_cw = (uint16_t)((g_fpu_cw & ~CW_X87_MASK) | cfp_to_cw(c));
    }
    CRET(cw_to_cfp(g_fpu_cw));
}

/* ---- the x87 helpers: operands on the lifted FPU stack, no stack arguments
 * (pcrecomp's x87crt.c): st(0) for the unary ones; st(0) = y over st(1) = x
 * for the binary ones. Computed in double. */
#define CI_UNARY(name, expr) static void c_##name(void) { double x = X87_POP(); X87_PUSH(expr); CRET(g_eax); }
#define CI_BINARY(name, expr) \
    static void c_##name(void) { double y = X87_POP(), x = X87_POP(); X87_PUSH(expr); CRET(g_eax); }
CI_UNARY(CIacos, acos(x))
CI_UNARY(CIasin, asin(x))
CI_UNARY(CIexp, exp(x))
CI_UNARY(CIsqrt, sqrt(x))
CI_UNARY(CIcosh, cosh(x))
CI_UNARY(CIsinh, sinh(x))
CI_UNARY(CItanh, tanh(x))
CI_BINARY(CIpow, pow(x, y))
CI_BINARY(CIfmod, fmod(x, y))

/* ---- runtime ---- */
/* _initterm(begin, end): each non-zero function pointer in [begin, end), in order. */
static void c_initterm(void) {
    uint32_t end = A32(1);
    for (uint32_t p = A32(0); p < end; p += 4)
        if (MEM32(p)) hle_call_guest(MEM32(p), 0, NULL);
    CRET(0);
}

/* setlocale(category, locale): the "C" locale only, which is all D3DX asks for.
 * NULL asks for the current one. Any other name fails (NULL), as an unknown
 * locale does. */
static void c_setlocale(void) {
    static uint32_t c_name;
    const char *l = ASTR(1);
    if (A32(0) > 5) CRET(0);                                  /* LC_ALL 0 .. LC_TIME 5 */
    if (l && strcmp(l, "C")) CRET(0);
    if (!c_name) c_name = hle_guest_strdup("C");
    CRET(c_name);
}

/* qsort(base, n, size, cmp): an insertion sort in guest memory with the guest
 * comparator (cdecl, two pointers). The order of equal elements is not promised. */
static void c_qsort(void) {
    uint32_t base = A32(0), n = A32(1), size = A32(2), cmp = A32(3);
    if (n < 2 || !size) CRET(0);
    uint8_t tmp[256], *t = size <= sizeof tmp ? tmp : malloc(size);
    if (!t) hle_fatal("msvcrt: qsort: no memory for a %u-byte element", size);
    uint32_t key = hle_va(hle_heap_alloc(size, 0));          /* the moving element, where cmp can read it */
    if (!key) hle_fatal("msvcrt: qsort: no guest memory");
    for (uint32_t i = 1; i < n; i++) {
        memcpy(hle_ptr(key), hle_ptr(base + i * size), size);
        uint32_t j = i;
        while (j > 0) {
            uint32_t args[2] = { base + (j - 1) * size, key };
            if ((int32_t)hle_call_guest(cmp, 2, args) <= 0) break;
            j--;
        }
        if (j != i) {
            memcpy(t, hle_ptr(key), size);
            memmove(hle_ptr(base + (j + 1) * size), hle_ptr(base + j * size), (size_t)(i - j) * size);
            memcpy(hle_ptr(base + j * size), t, size);
        }
    }
    hle_heap_free(hle_ptr(key));
    if (t != tmp) free(t);
    CRET(0);
}

/* ---- _vsnprintf ----
 * The format is msvcrt's: flags -+ #0, a width and precision (or *), the size
 * prefixes h, l, L, I32, I64, ll. A guest va_list is the address of the first
 * argument. As msvcrt prints them, an exponent has at least three digits
 * (1.5e+003) and %p is eight upper-case hex digits; %s is not cut at any
 * length. A non-finite double (msvcrt's 1.#INF form), %n, a wide %S/%C and an
 * unknown conversion stop loudly. */
struct out { char *p; size_t n, cap; };
static void out_put(struct out *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (o->n + n + 1 > cap) cap *= 2;
        char *p = realloc(o->p, cap);
        if (!p) hle_fatal("msvcrt: _vsnprintf: out of host memory");
        o->p = p, o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
/* Prints spec (a host printf conversion) of one value into o. */
#define OUT_FMT(o, spec, ...) do {                                                    \
        int len_ = snprintf(NULL, 0, spec, __VA_ARGS__);                               \
        if (len_ < 0) crt_die("_vsnprintf: a conversion the host cannot print");       \
        char *b_ = malloc((size_t)len_ + 1);                                           \
        if (!b_) hle_fatal("msvcrt: _vsnprintf: out of host memory");                  \
        snprintf(b_, (size_t)len_ + 1, spec, __VA_ARGS__);                             \
        out_put(o, b_, (size_t)len_);                                                  \
        free(b_);                                                                      \
    } while (0)

static void format_guest(struct out *o, const char *f, uint32_t args) {
    while (*f) {
        if (*f != '%') { const char *q = strchr(f, '%'); size_t n = q ? (size_t)(q - f) : strlen(f); out_put(o, f, n); f += n; continue; }
        if (f[1] == '%') { out_put(o, "%", 1); f += 2; continue; }
        char spec[48];
        size_t s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0", *f) && s < 10) spec[s++] = *f++;
        int width = -1, have_width = 0;
        for (int part = 0; part < 2; part++) {               /* width, then .precision */
            if (part) { if (*f != '.') break; spec[s++] = *f++; }
            if (*f == '*') {
                int v = (int32_t)MEM32(args);
                args += 4, f++;
                if (!part && v < 0) spec[s++] = '-', v = -v;    /* a negative width is '-' */
                if (!part) width = v, have_width = 1;
                s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", v);
            } else {
                if (!part && *f >= '0' && *f <= '9') width = atoi(f), have_width = 1;
                while (*f >= '0' && *f <= '9' && s < 36) spec[s++] = *f++;
            }
            if (s >= 36) crt_die("_vsnprintf: a width or precision too long");
        }
        int wide = 0;
        for (;;) {
            if (f[0] == 'l' && f[1] == 'l') wide = 1, f += 2;
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') wide = 1, f += 3;
            else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') f += 3;
            else if (*f == 'h' || *f == 'l' || *f == 'L') {
                if (f[1] == 's' || f[1] == 'c') {
                    if (*f == 'l') crt_die("_vsnprintf: a wide %ls or %lc");
                }
                f++;
            }
            else break;
        }
        char c = *f ? *f++ : 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (wide) {
                uint64_t v = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32;
                args += 8;
                spec[s++] = 'l', spec[s++] = 'l', spec[s++] = c, spec[s] = 0;
                if (c == 'd' || c == 'i') OUT_FMT(o, spec, (long long)v);
                else OUT_FMT(o, spec, (unsigned long long)v);
            } else {
                uint32_t v = MEM32(args);
                args += 4;
                spec[s++] = c, spec[s] = 0;
                if (c == 'd' || c == 'i') OUT_FMT(o, spec, (int)(int32_t)v);
                else OUT_FMT(o, spec, (unsigned)v);
            }
            break;
        case 'c':
            spec[s++] = 'c', spec[s] = 0;
            OUT_FMT(o, spec, (int)(uint8_t)MEM32(args));
            args += 4;
            break;
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            uint64_t bits = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32;
            double d;
            memcpy(&d, &bits, 8);
            args += 8;
            if (!isfinite(d)) crt_die("_vsnprintf: a non-finite double (msvcrt's 1.#INF form is not done)");
            spec[s++] = c, spec[s] = 0;
            int len = snprintf(NULL, 0, spec, d);
            char *b = malloc((size_t)len + 1);
            if (!b) hle_fatal("msvcrt: _vsnprintf: out of host memory");
            snprintf(b, (size_t)len + 1, spec, d);
            char *e = (c == 'f') ? NULL : strpbrk(b, "eE");
            if (e && (e[1] == '+' || e[1] == '-') && strlen(e + 2) == 2 + strspn(e + 4, " ")) {
                /* the host's two-digit exponent gets msvcrt's third digit: printed again
                   one narrower (any padding then gives a place to it), and a '0' put in
                   after the exponent's sign */
                if (have_width && width > 0) {
                    char sp2[48];
                    size_t k = 0;
                    const char *g = spec;
                    while (*g && strchr("%-+ #0", *g)) sp2[k++] = *g++;
                    k += (size_t)snprintf(sp2 + k, sizeof sp2 - k, "%d", width - 1);
                    while (*g >= '0' && *g <= '9') g++;
                    snprintf(sp2 + k, sizeof sp2 - k, "%s", g);
                    free(b);
                    len = snprintf(NULL, 0, sp2, d);
                    b = malloc((size_t)len + 2);
                    if (!b) hle_fatal("msvcrt: _vsnprintf: out of host memory");
                    snprintf(b, (size_t)len + 1, sp2, d);
                    e = strpbrk(b, "eE");
                } else {
                    char *nb = realloc(b, (size_t)len + 2);
                    if (!nb) hle_fatal("msvcrt: _vsnprintf: out of host memory");
                    b = nb;
                    e = strpbrk(b, "eE");
                }
                memmove(e + 3, e + 2, strlen(e + 2) + 1);
                e[2] = '0';
                len++;
            }
            out_put(o, b, (size_t)len);
            free(b);
            break;
        }
        case 's': {
            const char *str = (const char *)hle_ptr(MEM32(args));
            args += 4;
            spec[s++] = 's', spec[s] = 0;
            OUT_FMT(o, spec, str ? str : "(null)");
            break;
        }
        case 'p':
            OUT_FMT(o, "%08X", MEM32(args));
            args += 4;
            break;
        case 'n': crt_die("_vsnprintf: %n");
        case 'S': case 'C': crt_die("_vsnprintf: a wide %S or %C");
        default: {
            char what[64];
            snprintf(what, sizeof what, "_vsnprintf: an unknown conversion '%c'", c ? c : '0');
            crt_die(what);
        }
        }
    }
}

/* _vsnprintf(buf, n, fmt, va): for a text of length len, len < n stores it
 * with its terminator and gives len; len == n stores n bytes, no terminator,
 * and gives n; len > n stores the first n bytes, no terminator, and gives -1. */
static void c_vsnprintf(void) {
    uint32_t buf = A32(0), n = A32(1);
    struct out o = { NULL, 0, 0 };
    out_put(&o, "", 0);
    format_guest(&o, ASTR(2), A32(3));
    uint32_t len = (uint32_t)o.n;
    if (len < n) memcpy(hle_ptr(buf), o.p, (size_t)len + 1);
    else if (n) memcpy(hle_ptr(buf), o.p, n);
    free(o.p);
    CRET(len <= n ? len : 0xFFFFFFFFu);
}

/* _purecall and terminate: a pure virtual call, or a C++ exception nobody
 * catches. Neither has an answer; both stop loudly. */
static void c_purecall(void)  { crt_die("_purecall"); }
static void c_terminate(void) { crt_die("?terminate@@YAXXZ"); }

const win32hle_shim bw2_crt_shims[] = {
    { "malloc",        c_malloc },
    { "calloc",        c_calloc },
    { "realloc",       c_realloc },
    { "free",          c_free },
    { "??2@YAPAXI@Z",  c_malloc },
    { "??3@YAXPAX@Z",  c_free },
    { "_strdup",       c_strdup },
    { "strchr",        c_strchr },
    { "_stricmp",      c_stricmp },
    { "memmove",       c_memmove },
    { "wcslen",        c_wcslen },
    { "tolower",       c_tolower },
    { "toupper",       c_toupper },
    { "isalnum",       c_isalnum },
    { "isalpha",       c_isalpha },
    { "isdigit",       c_isdigit },
    { "isxdigit",      c_isxdigit },
    { "isspace",       c_isspace },
    { "atoi",          c_atoi },
    { "atol",          c_atoi },
    { "atof",          c_atof },
    { "floor",         c_floor },
    { "ceil",          c_ceil },
    { "ldexp",         c_ldexp },
    { "frexp",         c_frexp },
    { "modf",          c_modf },
    { "_finite",       c_finite },
    { "_isnan",        c_isnan },
    { "_fpclass",      c_fpclass },
    { "_controlfp",    c_controlfp },
    { "_CIacos",       c_CIacos },
    { "_CIasin",       c_CIasin },
    { "_CIexp",        c_CIexp },
    { "_CIsqrt",       c_CIsqrt },
    { "_CIcosh",       c_CIcosh },
    { "_CIsinh",       c_CIsinh },
    { "_CItanh",       c_CItanh },
    { "_CIpow",        c_CIpow },
    { "_CIfmod",       c_CIfmod },
    { "_initterm",     c_initterm },
    { "setlocale",     c_setlocale },
    { "_vsnprintf",    c_vsnprintf },
    { "qsort",         c_qsort },
    { "_purecall",     c_purecall },
    { "?terminate@@YAXXZ", c_terminate },
    { NULL, NULL }
};
