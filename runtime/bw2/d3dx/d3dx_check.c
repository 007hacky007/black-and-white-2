/*
 * d3dx_check.c - the lifted D3DX's checks (plan 9 Task 4), run by
 * `bw2 --game GAME --check-d3dx DIR` after the boot's setup, on the guest's
 * main thread. The checks are named on stdin, one per line; each prints
 * "<check>: ok" or "<check>: FAIL <why>", and a check may print
 * "<check>: <key> <value>" lines before that for the test to pin. Inputs
 * (HLSL and assembly texts) are files the test wrote into DIR. The exit code
 * is 0 only when every check passed.
 *
 * Calls go through the game's own imports (hle_resolve: the shims the game
 * binds to, the D3DXCompileShader forwarders among them) or straight to a
 * lifted export (d3dx_lifted_proc).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "d3d_com.h"
#include "d3dx.h"
#include "sm.h"

uint32_t bw2_validator_calls(int slot);                 /* d3dx_lifted.c */
int bw2_validator_callers(int slot, const uint32_t **out);
uint32_t bw2_debugsetmute_calls(void);
int bw2_lifted_dll_unanswered(const char ***names);     /* dll.c */

static const char *g_dir, *g_check;
static char g_why[512];
#define FAIL(...) do { snprintf(g_why, sizeof g_why, __VA_ARGS__); return 0; } while (0)
static void note(const char *key, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s: %s ", g_check, key);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

static uint32_t gmem(uint32_t n) { uint32_t p = hle_va(hle_alloc(n ? n : 4)); memset(hle_ptr(p), 0, n); return p; }
static uint32_t gstr(const char *s) { uint32_t p = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(p), s); return p; }
static uint32_t callv(uint32_t va, int n, va_list ap) {
    uint32_t a[16];
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    return hle_call_guest(va, n, a);
}
/* a game import (a shim) */
static uint32_t gcall(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    if (!va) { fprintf(stderr, "[bw2] FATAL: d3dx check: %s is not answered\n", name); abort(); }
    va_list ap;
    va_start(ap, n);
    uint32_t r = callv(va, n, ap);
    va_end(ap);
    return r;
}
/* a lifted D3DX export */
static uint32_t lcall(const char *name, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint32_t r = callv(d3dx_lifted_proc(name), n, ap);
    va_end(ap);
    return r;
}
static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return hle_call_guest(MEM32(MEM32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
/* A stdcall shim called as lifted code calls it, checking it popped exactly n arguments. */
static int scall(uint32_t va, int n, const uint32_t *a, uint32_t *eax) {
    recomp_func_t f = recomp_lookup_import(va);
    if (!f) return 0;
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    *eax = g_eax;
    int ok = g_esp == esp0;
    g_esp = esp0;
    return ok;
}
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* A file the test wrote into DIR, as guest bytes (NUL-ended); *n its length. */
static uint32_t input(const char *name, uint32_t *n) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[bw2] FATAL: d3dx check: no input %s\n", path); abort(); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t p = gmem((uint32_t)len + 1);
    *n = (uint32_t)fread(hle_ptr(p), 1, (size_t)len, f);
    fclose(f);
    return p;
}

/* ID3DXBuffer: slots 3 GetBufferPointer, 4 GetBufferSize. ID3DXConstantTable (d3dx9_25): 6 GetConstantDesc,
 * 9 GetConstantByName, 21 SetMatrix. IDirect3DDevice9: 95 GetVertexShaderConstantF. */
enum { BUF_PTR = 3, BUF_SIZE = 4, CT_DESC = 6, CT_BYNAME = 9, CT_SETMATRIX = 21, DEV_GETVSCONSTF = 95 };
#define D3DXSHADER_SKIPVALIDATION 2u

struct compiled { uint32_t hr, shader, errors, table; uint32_t *tok; uint32_t ntok; };
static void release(uint32_t obj) { if (obj) vcall(obj, 2, 0); }
static void drop(struct compiled *c) {
    release(c->shader), release(c->errors), release(c->table);
    free(c->tok);
    memset(c, 0, sizeof *c);
}
static void take_tokens(struct compiled *c) {
    if (!c->shader) return;
    uint32_t p = vcall(c->shader, BUF_PTR, 0), size = vcall(c->shader, BUF_SIZE, 0);
    c->ntok = size / 4;
    c->tok = malloc(size ? size : 4);
    memcpy(c->tok, hle_ptr(p), size);
}
/* D3DXCompileShader through the game's import (the forwarder) or the lifted export. */
static struct compiled compile(const char *file, const char *profile, uint32_t flags, int lifted) {
    struct compiled c = { 0 };
    uint32_t n, src = input(file, &n), out = gmem(12);
    uint32_t a[10] = { src, n, 0, 0, gstr("main"), gstr(profile), flags, out, out + 4, out + 8 };
    c.hr = hle_call_guest(lifted ? d3dx_lifted_proc("D3DXCompileShader") : hle_resolve("D3DXCompileShader"), 10, a);
    c.shader = MEM32(out), c.errors = MEM32(out + 4), c.table = MEM32(out + 8);
    take_tokens(&c);
    return c;
}
static int has_ctab(const struct compiled *c) {
    for (uint32_t i = 1; i + 1 < c->ntok; i++)
        if ((c->tok[i] & 0xFFFFu) == 0xFFFEu && c->tok[i + 1] == 0x42415443u) return 1;   /* "CTAB" */
    return 0;
}
/* The tokens with comment blocks taken out. */
static uint32_t strip(const uint32_t *t, uint32_t n, uint32_t *out) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i && !(t[i] & 0x80000000u) && (t[i] & 0xFFFFu) == 0xFFFEu) { i += (t[i] >> 16) & 0x7FFFu; continue; }
        out[k++] = t[i];
    }
    return k;
}
static const char *errtext(uint32_t errors) { return errors ? (const char *)hle_ptr(vcall(errors, BUF_PTR, 0)) : ""; }

/* ---- the checks ---- */
static int c_d3dx_load(void) {
    uint32_t a = d3dx_lifted_proc("D3DXGetShaderVersion"), b = d3dx_lifted_proc("D3DXGetShaderVersion");
    if (!a || a != b || a < 0x08000000u || a >= 0x08253000u) FAIL("D3DXGetShaderVersion at 0x%08X, then 0x%08X", a, b);
    uint32_t tok = gmem(8);
    MEM32(tok) = 0xFFFF0200u, MEM32(tok + 4) = 0x0000FFFFu;
    uint32_t v = lcall("D3DXGetShaderVersion", 1, tok);
    if (v != 0xFFFF0200u) FAIL("D3DXGetShaderVersion gave 0x%08X", v);
    const char **names;
    int n = bw2_lifted_dll_unanswered(&names);
    char list[4096] = "";
    for (int i = 0; i < n; i++) {
        size_t k = strlen(list);
        snprintf(list + k, sizeof list - k, "%s%s", i ? " " : "", names[i]);
    }
    note("unanswered", "%s", list);
    return 1;
}

/* The IAT slot of dll!name in the image at base, or 0. */
static uint32_t iat_slot(uint32_t base, const char *dll, const char *name) {
    uint32_t nt = base + MEM32(base + 0x3C), imp = MEM32(nt + 0x80);
    for (uint32_t d = base + imp; imp && MEM32(d + 12); d += 20) {
        if (strcasecmp((const char *)hle_ptr(base + MEM32(d + 12)), dll)) continue;
        uint32_t ilt = base + (MEM32(d) ? MEM32(d) : MEM32(d + 16)), iat = base + MEM32(d + 16);
        for (uint32_t k = 0; MEM32(ilt + 4 * k); k++) {
            uint32_t e = MEM32(ilt + 4 * k);
            if (!(e & 0x80000000u) && !strcmp((const char *)hle_ptr(base + e + 2), name)) return iat + 4 * k;
        }
    }
    return 0;
}
static int c_d3dx_data_import(void) {
    d3dx_lifted_proc("D3DXGetShaderVersion");
    uint32_t slot = iat_slot(0x08000000u, "msvcrt.dll", "_adjust_fdiv");
    if (!slot) FAIL("no IAT slot for msvcrt!_adjust_fdiv");
    uint32_t v = MEM32(slot);
    if (!v || v >= 0x7F000000u || hle_is_stub(v) || hle_name(v)) FAIL("_adjust_fdiv's slot holds 0x%08X", v);
    if (MEM32(v)) FAIL("_adjust_fdiv is %u, not 0", MEM32(v));
    return 1;
}

static int c_d3dx_module_name(void) {
    uint32_t name = gstr("d3dx9_25.dll"), lifted = gstr("bw2-d3dx9_25-lifted.dll");
    gcall("SetLastError", 1, 0u);
    uint32_t h1 = gcall("GetModuleHandleA", 1, name), e1 = gcall("GetLastError", 0);
    d3dx_lifted_proc("D3DXGetShaderVersion");
    gcall("SetLastError", 1, 0u);
    uint32_t h2 = gcall("GetModuleHandleA", 1, name), e2 = gcall("GetLastError", 0);
    note("game", "0x%08X %u", h1, e1);
    if (h1 != h2 || e1 != e2) FAIL("GetModuleHandleA(d3dx9_25.dll) 0x%08X/%u before, 0x%08X/%u after", h1, e1, h2, e2);
    uint32_t h3 = gcall("GetModuleHandleA", 1, lifted);
    if (h3 != 0x08000000u) FAIL("GetModuleHandleA(bw2-d3dx9_25-lifted.dll) 0x%08X", h3);
    return 1;
}

/* the lifted functions Step 3 found calling each validator slot (docs/research/hlsl.md). g_cur_func names
 * the function whose lifted code runs, so a function that tail-jumps into a site is named in its place:
 * sub_0815B4B9 ends with a jmp to sub_08149A1B. */
static const uint32_t g_ret_begin[] = { 0x080F3F99u, 0x08149A1Bu, 0x0815B4B9u, 0x08165B62u, 0 };
static const uint32_t g_ret_instr[] = { 0x080F17EAu, 0x08143905u, 0x08165B62u, 0 };
static const uint32_t g_ret_end[] = { 0x080F3F99u, 0x08149A1Bu, 0x0815B4B9u, 0x08165B62u, 0 };
static const uint32_t g_ret_release[] = { 0x080F3F99u, 0x08149A1Bu, 0x0815B4B9u, 0x08165B62u, 0 };
static int callers_known(int slot, const uint32_t *known) {
    const uint32_t *got;
    int n = bw2_validator_callers(slot, &got);
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (known[k] && known[k] != got[i]) k++;
        if (!known[k]) { snprintf(g_why, sizeof g_why, "validator slot %d called from sub_%08X", slot, got[i]); return 0; }
    }
    if (n) note(slot == 3 ? "begin_from" : slot == 4 ? "instruction_from" : slot == 5 ? "end_from" : "release_from",
                "sub_%08X%s", got[0], n > 1 ? " and more" : "");
    return 1;
}
static int c_d3dx_validator(void) {
    uint32_t d3d9n = gstr("d3d9.dll"), procn = gstr("Direct3DShaderValidatorCreate9");
    uint32_t d3d9 = gcall("GetModuleHandleA", 1, d3d9n);
    if (!d3d9) FAIL("GetModuleHandleA(d3d9.dll) is 0");
    uint32_t create = gcall("GetProcAddress", 2, d3d9, procn);
    if (!create || create != hle_resolve("Direct3DShaderValidatorCreate9")) FAIL("GetProcAddress gave 0x%08X", create);

    uint32_t c0 = bw2_validator_calls(-1), b0 = bw2_validator_calls(3), i0 = bw2_validator_calls(4);
    uint32_t e0 = bw2_validator_calls(5), r0 = bw2_validator_calls(2);
    struct compiled v = compile("ps.hlsl", "ps_2_0", 0, 0);
    uint32_t c1 = bw2_validator_calls(-1), b1 = bw2_validator_calls(3), i1 = bw2_validator_calls(4);
    uint32_t e1 = bw2_validator_calls(5), r1 = bw2_validator_calls(2);
    note("flags0", "created %u begin %u instruction %u end %u release %u", c1 - c0, b1 - b0, i1 - i0, e1 - e0, r1 - r0);
    if (v.hr) FAIL("flags 0: 0x%08X %s", v.hr, errtext(v.errors));
    if (!callers_known(3, g_ret_begin) || !callers_known(4, g_ret_instr) || !callers_known(5, g_ret_end) ||
        !callers_known(2, g_ret_release))
        return 0;
    if (bw2_validator_calls(0) || bw2_validator_calls(1)) FAIL("D3DX called QueryInterface or AddRef");
    struct compiled s = compile("ps.hlsl", "ps_2_0", D3DXSHADER_SKIPVALIDATION, 0);
    note("skip", "created %u", bw2_validator_calls(-1) - c1);
    if (s.hr) FAIL("SKIPVALIDATION: 0x%08X %s", s.hr, errtext(s.errors));
    /* the same shader; the CTAB comment records the compile flags, so comments are compared apart */
    uint32_t *sv = malloc(4 * v.ntok + 4), *ss = malloc(4 * s.ntok + 4);
    uint32_t nv = strip(v.tok, v.ntok, sv), ns = strip(s.tok, s.ntok, ss), diff = 0;
    for (uint32_t i = 0; i < v.ntok && i < s.ntok; i++) diff += v.tok[i] != s.tok[i];
    int same = nv == ns && !memcmp(sv, ss, 4 * nv);
    free(sv), free(ss);
    note("skip_tokens", "%u and %u, %u without comments, %u differ", v.ntok, s.ntok, nv, diff);
    if (!same) FAIL("SKIPVALIDATION gave another shader (%u and %u tokens without comments)", nv, ns);
    drop(&v), drop(&s);
    note("debugsetmute", "%u", bw2_debugsetmute_calls());

    /* the validator's own methods, called as D3DX calls them: argument counts and IUnknown */
    uint32_t eax, obj;
    if (!scall(create, 0, NULL, &obj) || !obj) FAIL("Direct3DShaderValidatorCreate9 popped wrongly or gave 0");
    uint32_t vt = MEM32(obj), out = gmem(4), iid = gmem(16), cb = gmem(4);
    static const uint8_t iid_unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    memcpy(hle_ptr(iid), iid_unknown, 16);
    uint32_t qi[3] = { obj, iid, out };
    if (!scall(MEM32(vt), 3, qi, &eax) || eax || MEM32(out) != obj) FAIL("QueryInterface(IUnknown) 0x%08X", eax);
    MEM8(iid) = 1;
    if (!scall(MEM32(vt), 3, qi, &eax) || eax != E_NOINTERFACE_ || MEM32(out)) FAIL("QueryInterface(other) 0x%08X", eax);
    uint32_t self[1] = { obj }, begin[4] = { obj, cb, 0, 0 }, ins[5] = { obj, 0, 0, cb, 1 };
    if (!scall(MEM32(vt + 4), 1, self, &eax) || eax != 3) FAIL("AddRef gave %u", eax);
    if (!scall(MEM32(vt + 12), 4, begin, &eax) || eax) FAIL("Begin");
    if (!scall(MEM32(vt + 16), 5, ins, &eax) || eax) FAIL("Instruction");
    if (!scall(MEM32(vt + 20), 1, self, &eax) || eax) FAIL("End");
    for (uint32_t want = 2; want != (uint32_t)-1; want--)
        if (!scall(MEM32(vt + 8), 1, self, &eax) || eax != want) FAIL("Release gave %u, want %u", eax, want);
    for (int i = 0; i < 6; i++) {
        const char *nm = hle_name(MEM32(vt + 4u * (uint32_t)i));
        for (int j = 0; j < i; j++)
            if (MEM32(vt + 4u * (uint32_t)j) == MEM32(vt + 4u * (uint32_t)i)) FAIL("slots %d and %d share %s", j, i, nm);
    }
    return 1;
}

static int c_d3dx_cpu(void) {
    d3dx_lifted_proc("D3DXMatrixMultiply");
    note("initial", "0x%08X", MEM32(0x0821129Cu));
    float m1[16], m2[16], want[16];
    for (int i = 0; i < 16; i++) m1[i] = (float)(i + 1), m2[i] = (float)((i * 7) % 5) - 1.5f;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += m1[4 * r + k] * m2[4 * k + c];
            want[4 * r + c] = s;
        }
    uint32_t a = gmem(64), b = gmem(64), o = gmem(64);
    for (int i = 0; i < 16; i++) MEM32(a + 4u * (uint32_t)i) = f2u(m1[i]), MEM32(b + 4u * (uint32_t)i) = f2u(m2[i]);
    uint32_t r = lcall("D3DXMatrixMultiply", 3, o, a, b);
    note("selection", "0x%08X", MEM32(0x0821129Cu));
    if (r != o) FAIL("D3DXMatrixMultiply returned 0x%08X, not its output 0x%08X", r, o);
    for (int i = 0; i < 16; i++)
        if (u2f(MEM32(o + 4u * (uint32_t)i)) != want[i]) FAIL("element %d is %g, want %g", i, u2f(MEM32(o + 4u * (uint32_t)i)), want[i]);
    return 1;
}

static int describe(uint32_t table, const char *name, uint32_t *set, uint32_t *index, uint32_t *count) {
    uint32_t h = vcall(table, CT_BYNAME, 2, 0u, gstr(name));
    if (!h) { snprintf(g_why, sizeof g_why, "GetConstantByName(%s) is 0", name); return 0; }
    uint32_t desc = gmem(48), n = gmem(4);
    MEM32(n) = 1;
    uint32_t hr = vcall(table, CT_DESC, 3, h, desc, n);
    if (hr || MEM32(n) != 1) { snprintf(g_why, sizeof g_why, "GetConstantDesc(%s) 0x%08X, %u", name, hr, MEM32(n)); return 0; }
    *set = MEM32(desc + 4), *index = MEM32(desc + 8), *count = MEM32(desc + 12);
    return 1;
}
static int c_hlsl_compile_ps(void) {
    struct compiled c = compile("ps.hlsl", "ps_2_0", 0, 0);
    if (c.hr) FAIL("0x%08X %s", c.hr, errtext(c.errors));
    if (!c.ntok || c.tok[0] != 0xFFFF0200u) FAIL("first token 0x%08X", c.ntok ? c.tok[0] : 0);
    if (!has_ctab(&c)) FAIL("no CTAB comment");
    if (!c.table) FAIL("no constant table");
    uint32_t set, index, count;
    if (!describe(c.table, "c", &set, &index, &count)) return 0;
    if (set != 2 || index != 0 || count != 1) FAIL("c: set %u, register %u, count %u", set, index, count);
    struct sm_shader s;
    char err[256];
    if (sm_decode(c.tok, c.ntok, &s, err, sizeof err)) FAIL("sm_decode: %s", err);
    sm_free(&s);
    drop(&c);
    return 1;
}

static int c_hlsl_compile_vs(void) {
    struct compiled c = compile("vs.hlsl", "vs_1_1", 0, 0);
    if (c.hr) FAIL("0x%08X %s", c.hr, errtext(c.errors));
    if (!c.ntok || c.tok[0] != 0xFFFE0101u) FAIL("first token 0x%08X", c.ntok ? c.tok[0] : 0);
    struct sm_shader s;
    char err[256];
    if (sm_decode(c.tok, c.ntok, &s, err, sizeof err)) FAIL("sm_decode: %s", err);
    uint32_t dp4 = 0;
    for (uint32_t i = 0; i < s.nins; i++) dp4 += s.ins[i].op == SM_DP4;
    sm_free(&s);
    drop(&c);
    if (dp4 != 4) FAIL("%u dp4 instructions", dp4);
    return 1;
}

static int c_hlsl_compile_error(void) {
    struct compiled c = compile("error.hlsl", "ps_2_0", 0, 0);
    if (!(c.hr & 0x80000000u)) FAIL("hr 0x%08X", c.hr);
    if (c.shader) FAIL("a shader 0x%08X", c.shader);
    if (!c.errors || !strstr(errtext(c.errors), "undefined_thing")) FAIL("error buffer 0x%08X", c.errors);
    drop(&c);
    return 1;
}

static int c_hlsl_asm_matches_native(void) {
    static const char *const files[] = { "ps11.asm", "ps20.asm", "vs11.asm" };
    for (int f = 0; f < 3; f++) {
        struct compiled c[2] = { { 0 }, { 0 } };
        for (int lifted = 0; lifted < 2; lifted++) {
            uint32_t n, src = input(files[f], &n), out = gmem(8);
            uint32_t a[7] = { src, n, 0, 0, 0, out, out + 4 };
            c[lifted].hr = hle_call_guest(lifted ? d3dx_lifted_proc("D3DXAssembleShader") : hle_resolve("D3DXAssembleShader"), 7, a);
            c[lifted].shader = MEM32(out), c[lifted].errors = MEM32(out + 4);
            take_tokens(&c[lifted]);
            if (c[lifted].hr) FAIL("%s, %s: 0x%08X %s", files[f], lifted ? "lifted" : "native", c[lifted].hr, errtext(c[lifted].errors));
        }
        uint32_t *s0 = malloc(4 * c[0].ntok + 4), *s1 = malloc(4 * c[1].ntok + 4);
        uint32_t n0 = strip(c[0].tok, c[0].ntok, s0), n1 = strip(c[1].tok, c[1].ntok, s1);
        int same = n0 == n1 && !memcmp(s0, s1, 4 * n0);
        uint32_t at = 0;
        while (at < n0 && at < n1 && s0[at] == s1[at]) at++;
        free(s0), free(s1);
        drop(&c[0]), drop(&c[1]);
        if (!same) FAIL("%s: native %u tokens, lifted %u, first difference at %u", files[f], n0, n1, at);
    }
    return 1;
}

static uint32_t new_device(void) {
    uint32_t d3d = gcall("Direct3DCreate9", 1, 32u);
    uint32_t pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = 640, MEM32(pp + 4) = 480, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1, MEM32(pp + 36) = 1, MEM32(pp + 40) = 75;
    uint32_t hr = d3d ? vcall(d3d, 16, 6, 0u, 1u, 0x10001u, 0x40u, pp, out) : 1;
    return hr ? 0 : MEM32(out);
}
static int c_hlsl_constant_table(void) {
    uint32_t dev = new_device();
    if (!dev) FAIL("no device");
    struct compiled c = compile("vs.hlsl", "vs_1_1", 0, 0);
    if (c.hr || !c.table) FAIL("compile 0x%08X %s", c.hr, errtext(c.errors));
    uint32_t set, index, count;
    if (!describe(c.table, "m", &set, &index, &count)) return 0;
    if (set != 2 || index != 0 || count != 4) FAIL("m: set %u, register %u, count %u", set, index, count);
    uint32_t h = vcall(c.table, CT_BYNAME, 2, 0u, gstr("m")), m = gmem(64), regs = gmem(64);
    for (uint32_t r = 0; r < 4; r++)
        for (uint32_t k = 0; k < 4; k++) MEM32(m + 16 * r + 4 * k) = f2u((float)(4 * r + k + 1));
    uint32_t hr = vcall(c.table, CT_SETMATRIX, 3, dev, h, m);
    if (hr) FAIL("SetMatrix 0x%08X", hr);
    hr = vcall(dev, DEV_GETVSCONSTF, 3, 0u, regs, 4u);
    if (hr) FAIL("GetVertexShaderConstantF 0x%08X", hr);
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t r = 0; r < 4; r++) {
            float got = u2f(MEM32(regs + 16 * i + 4 * r)), want = (float)(4 * r + i + 1);
            if (got != want) FAIL("c%u.%c is %g, want %g", i, "xyzw"[r], got, want);
        }
    drop(&c);
    release(dev);
    return 1;
}

/* Two guest threads, each compiling the ps_2_0 shader 20 times through the lifted export. */
#define THREAD_COMPILES 20
static uint32_t *g_ref_tok, g_ref_ntok, g_thread_ok[2], g_thread_done[2];
static void t_compile_thread(void) {                     /* (index): stdcall thread start */
    uint32_t idx = A32(0) & 1, ok = 0;
    for (int i = 0; i < THREAD_COMPILES; i++) {
        struct compiled c = compile("ps.hlsl", "ps_2_0", 0, 1);
        ok += !c.hr && c.ntok == g_ref_ntok && !memcmp(c.tok, g_ref_tok, 4 * g_ref_ntok);
        drop(&c);
    }
    g_thread_ok[idx] = ok, g_thread_done[idx] = 1;
    RET(0, 1);
}
static const win32hle_shim g_thread_tab[] = { { "bw2_d3dx_check_compile_thread", t_compile_thread }, { 0, 0 } };
static int c_hlsl_two_threads(void) {
    struct compiled ref = compile("ps.hlsl", "ps_2_0", 0, 1);
    if (ref.hr) FAIL("single compile 0x%08X %s", ref.hr, errtext(ref.errors));
    g_ref_tok = ref.tok, g_ref_ntok = ref.ntok;
    uint32_t start = MEM32(hle_com_vtable(g_thread_tab)), tid = gmem(4), handles = gmem(8);
    for (uint32_t i = 0; i < 2; i++) {
        MEM32(handles + 4 * i) = gcall("CreateThread", 6, 0u, 0u, start, i, 0u, tid);
        if (!MEM32(handles + 4 * i)) FAIL("CreateThread %u", i);
    }
    uint32_t w = gcall("WaitForMultipleObjects", 4, 2u, handles, 1u, 30000u);
    if (w != 0) FAIL("WaitForMultipleObjects gave 0x%08X (done %u %u)", w, g_thread_done[0], g_thread_done[1]);
    note("results", "%u %u of %u", g_thread_ok[0], g_thread_ok[1], THREAD_COMPILES);
    if (g_thread_ok[0] != THREAD_COMPILES || g_thread_ok[1] != THREAD_COMPILES) FAIL("a result differed");
    ref.tok = NULL;
    drop(&ref);
    free(g_ref_tok);
    return 1;
}

static int c_hlsl_from_file(void) {
    struct compiled mem = compile("ps.hlsl", "ps_2_0", 0, 0), file = { 0 };
    if (mem.hr) FAIL("from memory 0x%08X", mem.hr);
    uint32_t out = gmem(12);
    file.hr = gcall("D3DXCompileShaderFromFileA", 9, gstr("C:\\hlsl_from_file.hlsl"), 0u, 0u, gstr("main"),
                    gstr("ps_2_0"), 0u, out, out + 4, out + 8);
    file.shader = MEM32(out), file.errors = MEM32(out + 4), file.table = MEM32(out + 8);
    take_tokens(&file);
    if (file.hr) FAIL("from file 0x%08X %s", file.hr, errtext(file.errors));
    if (file.ntok != mem.ntok || memcmp(file.tok, mem.tok, 4 * mem.ntok)) FAIL("other tokens (%u, %u)", file.ntok, mem.ntok);
    drop(&mem), drop(&file);
    return 1;
}

static const struct { const char *name; int (*fn)(void); } g_checks[] = {
    { "d3dx_load", c_d3dx_load }, { "d3dx_data_import", c_d3dx_data_import },
    { "d3dx_module_name", c_d3dx_module_name }, { "d3dx_validator", c_d3dx_validator }, { "d3dx_cpu", c_d3dx_cpu },
    { "hlsl_compile_ps", c_hlsl_compile_ps }, { "hlsl_compile_vs", c_hlsl_compile_vs },
    { "hlsl_compile_error", c_hlsl_compile_error }, { "hlsl_asm_matches_native", c_hlsl_asm_matches_native },
    { "hlsl_constant_table", c_hlsl_constant_table }, { "hlsl_two_threads", c_hlsl_two_threads },
    { "hlsl_from_file", c_hlsl_from_file },
};

int bw2_d3dx_check(const char *dir) {
    g_dir = dir;
    mach_enter();
    int failed = 0;
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        size_t i = 0;
        while (i < sizeof g_checks / sizeof *g_checks && strcmp(g_checks[i].name, line)) i++;
        g_check = line;
        g_why[0] = 0;
        if (i == sizeof g_checks / sizeof *g_checks) printf("%s: FAIL no such check\n", line), failed++;
        else if (g_checks[i].fn()) printf("%s: ok\n", line);
        else printf("%s: FAIL %s\n", line, g_why), failed++;
        fflush(stdout);
    }
    mach_leave();
    return failed ? 1 : 0;
}
