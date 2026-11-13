/*
 * d3dx_check.c - the lifted D3DX's checks (plan 9 Task 4), run by
 * `bw2 --game GAME --check-d3dx DIR` after the boot's setup, on the guest's
 * main thread. The checks are named on stdin, one per line; each prints
 * "<check>: ok" or "<check>: FAIL <why>", and a check may print
 * "<check>: <key> <value>" lines before that for the test to pin. Inputs
 * (HLSL and assembly texts) are files the test wrote into DIR. The exit code
 * is 0 only when every check passed.
 *
 * Task 5 adds HLSL effects (D3DXCreateEffect with text), the fragment linker
 * and D3DXCreateSphere, the last two handing out the lifted DLL's own objects.
 *
 * Task 6 checks the lifted compiler against the game's own inputs: a check's
 * line may carry arguments after its name ("sdv_oracle <game dir> <name>...",
 * "fxsrc_compile <game dir> <item>..."), and those two read the game's files
 * in memory and print numbers only.
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
#include "fx.h"
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

/* ---- Task 5: HLSL effects, the fragment linker and the sphere ---- */
uint32_t d3d_draw_calls(void);                           /* d3d9_draw.c */

/* ID3DXEffect (d3dx9_25: 57 ID3DXBaseEffect slots, then the effect's own); IDirect3DDevice9; ID3DXMesh;
 * ID3DXFragmentLinker */
enum { FX_GETDESC = 3, FX_GETPARAMBYNAME = 9, FX_GETTECHBYNAME = 13, FX_GETFLOATARRAY = 33, FX_SETVECTOR = 34,
       FX_GETVECTOR = 35, FX_SETTECHNIQUE = 58, FX_BEGIN = 63, FX_BEGINPASS = 64, FX_ENDPASS = 66, FX_END = 67 };
enum { DEV_BEGINSCENE = 41, DEV_ENDSCENE = 42, DEV_SETRS = 57, DEV_GETRS = 58, DEV_GETSS = 68, DEV_SETSS = 69,
       DEV_SETVS = 92, DEV_GETVS = 93, DEV_SETPS = 107, DEV_GETPS = 108 };
enum { MESH_DRAWSUBSET = 3, MESH_NUMFACES = 4, MESH_NUMVERTICES = 5, MESH_FVF = 6, MESH_VB = 13 };
enum { FL_NUMFRAGMENTS = 4, FL_HANDLEBYNAME = 6, FL_ADDFRAGMENTS = 8, FL_LINKVS = 12 };
enum { RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_ALPHABLENDENABLE = 27, SS_ADDRESSU = 1, SS_MINFILTER = 6 };

struct effect { uint32_t hr, effect, errors; };
/* D3DXCreateEffect through the game's import, with the text the test wrote */
static struct effect create_effect(uint32_t dev, const char *file, uint32_t pool, int want_errors) {
    struct effect e = { 0 };
    uint32_t n, src = input(file, &n), out = gmem(8);
    MEM32(out) = MEM32(out + 4) = 0xCDCDCDCDu;           /* both must be written */
    e.hr = gcall("D3DXCreateEffect", 9, dev, src, n, 0u, 0u, 0u, pool, out, want_errors ? out + 4 : 0u);
    e.effect = MEM32(out), e.errors = want_errors ? MEM32(out + 4) : 0;
    return e;
}
static uint32_t rs_get(uint32_t dev, uint32_t st) { uint32_t v = gmem(4); vcall(dev, DEV_GETRS, 2, st, v); return MEM32(v); }
static uint32_t ss_get(uint32_t dev, uint32_t st) { uint32_t v = gmem(4); vcall(dev, DEV_GETSS, 3, 0u, st, v); return MEM32(v); }
/* the device's current shader (slot DEV_GETVS or DEV_GETPS), released again; 0 for none */
static uint32_t shader_get(uint32_t dev, int slot) {
    uint32_t v = gmem(4);
    if (vcall(dev, slot, 1, v)) return 0xFFFFFFFFu;
    if (MEM32(v)) release(MEM32(v));
    return MEM32(v);
}
static int kind_is(uint32_t obj, enum d3d_kind kind) {
    struct d3d_obj *o = obj ? d3d_host_any(obj) : NULL;
    return o && o->kind == kind;
}

static int c_hlsl_effect(void) {
    uint32_t dev = new_device();
    if (!dev) FAIL("no device");
    vcall(dev, DEV_SETRS, 2, (uint32_t)RS_ZENABLE, 1u);
    vcall(dev, DEV_SETSS, 3, 0u, (uint32_t)SS_MINFILTER, 1u), vcall(dev, DEV_SETSS, 3, 0u, (uint32_t)SS_ADDRESSU, 1u);
    vcall(dev, DEV_SETVS, 1, 0u), vcall(dev, DEV_SETPS, 1, 0u);
    struct effect e = create_effect(dev, "effect.fx", 0, 1);
    if (e.hr || !e.effect) FAIL("D3DXCreateEffect 0x%08X, effect 0x%08X: %s", e.hr, e.effect, errtext(e.errors));
    if (!kind_is(e.effect, D3DK_EFFECT)) FAIL("the effect 0x%08X is not ours", e.effect);
    uint32_t desc = gmem(16);
    if (vcall(e.effect, FX_GETDESC, 1, desc)) FAIL("GetDesc");
    note("desc", "techniques %u parameters %u", MEM32(desc + 8), MEM32(desc + 4));
    uint32_t k = vcall(e.effect, FX_GETPARAMBYNAME, 2, 0u, gstr("k")), f = gmem(16);
    if (!k || vcall(e.effect, FX_GETFLOATARRAY, 3, k, f, 4u)) FAIL("k: handle 0x%08X", k);
    static const float want[4] = { 1, 0.5f, 0.25f, 1 };
    for (uint32_t i = 0; i < 4; i++)
        if (u2f(MEM32(f + 4 * i)) != want[i]) FAIL("k[%u] is %g", i, u2f(MEM32(f + 4 * i)));
    uint32_t t = vcall(e.effect, FX_GETTECHBYNAME, 1, gstr("T")), np = gmem(4);
    if (!t || vcall(e.effect, FX_SETTECHNIQUE, 1, t)) FAIL("technique T 0x%08X", t);
    if (vcall(e.effect, FX_BEGIN, 2, np, 0u) || MEM32(np) != 1) FAIL("Begin: %u passes", MEM32(np));
    if (vcall(e.effect, FX_BEGINPASS, 1, 0u)) FAIL("BeginPass");
    uint32_t vs = shader_get(dev, DEV_GETVS), ps = shader_get(dev, DEV_GETPS);
    if (!kind_is(vs, D3DK_VSHADER) || !kind_is(ps, D3DK_PSHADER)) FAIL("in the pass: shaders 0x%08X 0x%08X", vs, ps);
    uint32_t z = rs_get(dev, RS_ZENABLE), mf = ss_get(dev, SS_MINFILTER), au = ss_get(dev, SS_ADDRESSU);
    if (z != 0 || mf != 2 || au != 3) FAIL("in the pass: ZENABLE %u, MINFILTER %u, ADDRESSU %u", z, mf, au);
    if (vcall(e.effect, FX_ENDPASS, 0) || vcall(e.effect, FX_END, 0)) FAIL("EndPass/End");
    vs = shader_get(dev, DEV_GETVS), ps = shader_get(dev, DEV_GETPS);
    z = rs_get(dev, RS_ZENABLE), mf = ss_get(dev, SS_MINFILTER), au = ss_get(dev, SS_ADDRESSU);
    if (vs || ps || z != 1 || mf != 1 || au != 1)
        FAIL("after End: shaders 0x%08X 0x%08X, ZENABLE %u, MINFILTER %u, ADDRESSU %u", vs, ps, z, mf, au);
    release(e.effect), release(e.errors), release(dev);
    return 1;
}

static int c_hlsl_effect_stateblock(void) {
    uint32_t dev = new_device();
    if (!dev) FAIL("no device");
    vcall(dev, DEV_SETRS, 2, (uint32_t)RS_ZWRITEENABLE, 1u), vcall(dev, DEV_SETRS, 2, (uint32_t)RS_ALPHABLENDENABLE, 1u);
    struct effect e = create_effect(dev, "effect_sb.fx", 0, 1);
    if (e.hr || !e.effect) FAIL("D3DXCreateEffect 0x%08X: %s", e.hr, errtext(e.errors));
    uint32_t np = gmem(4);
    if (vcall(e.effect, FX_BEGIN, 2, np, 0u) || MEM32(np) != 1 || vcall(e.effect, FX_BEGINPASS, 1, 0u)) FAIL("Begin");
    uint32_t zw = rs_get(dev, RS_ZWRITEENABLE), ab = rs_get(dev, RS_ALPHABLENDENABLE);
    uint32_t vs = shader_get(dev, DEV_GETVS), ps = shader_get(dev, DEV_GETPS);
    if (zw || ab || !kind_is(vs, D3DK_VSHADER) || !kind_is(ps, D3DK_PSHADER))
        FAIL("in the pass: ZWRITEENABLE %u, ALPHABLENDENABLE %u, shaders 0x%08X 0x%08X", zw, ab, vs, ps);
    if (vcall(e.effect, FX_ENDPASS, 0) || vcall(e.effect, FX_END, 0)) FAIL("EndPass/End");
    zw = rs_get(dev, RS_ZWRITEENABLE), ab = rs_get(dev, RS_ALPHABLENDENABLE);
    if (zw != 1 || ab != 1) FAIL("after End: ZWRITEENABLE %u, ALPHABLENDENABLE %u", zw, ab);
    release(e.effect), release(e.errors), release(dev);
    return 1;
}

static int vec_is(uint32_t effect, const char *name, float x, float y, float z, float w) {
    uint32_t h = vcall(effect, FX_GETPARAMBYNAME, 2, 0u, gstr(name)), v = gmem(16);
    if (!h || vcall(effect, FX_GETVECTOR, 2, h, v)) { snprintf(g_why, sizeof g_why, "%s: handle 0x%08X", name, h); return 0; }
    float got[4] = { u2f(MEM32(v)), u2f(MEM32(v + 4)), u2f(MEM32(v + 8)), u2f(MEM32(v + 12)) };
    if (got[0] != x || got[1] != y || got[2] != z || got[3] != w) {
        snprintf(g_why, sizeof g_why, "%s is %g %g %g %g, want %g %g %g %g", name, got[0], got[1], got[2], got[3], x, y, z, w);
        return 0;
    }
    return 1;
}
static int c_hlsl_effect_pool(void) {
    uint32_t dev = new_device(), out = gmem(4);
    if (!dev) FAIL("no device");
    if (gcall("D3DXCreateEffectPool", 1, out)) FAIL("D3DXCreateEffectPool");
    uint32_t pool = MEM32(out);
    struct effect a = create_effect(dev, "pool_a.fx", pool, 1), b = create_effect(dev, "pool_b.fx", pool, 1);
    if (a.hr || !a.effect) FAIL("effect a 0x%08X: %s", a.hr, errtext(a.errors));
    if (b.hr || !b.effect) FAIL("effect b 0x%08X: %s", b.hr, errtext(b.errors));
    if (!vec_is(b.effect, "g", 1, 2, 3, 4)) return 0;     /* the first effect's default is the pool's value */
    uint32_t h = vcall(a.effect, FX_GETPARAMBYNAME, 2, 0u, gstr("g")), v = gmem(16);
    MEM32(v) = f2u(5), MEM32(v + 4) = f2u(6), MEM32(v + 8) = f2u(7), MEM32(v + 12) = f2u(8);
    if (!h || vcall(a.effect, FX_SETVECTOR, 2, h, v)) FAIL("SetVector on a");
    if (!vec_is(b.effect, "g", 5, 6, 7, 8) || !vec_is(a.effect, "g", 5, 6, 7, 8)) return 0;
    if (!vec_is(a.effect, "a", 9, 9, 9, 9)) return 0;
    release(a.effect), release(b.effect), release(a.errors), release(b.errors), release(pool), release(dev);
    return 1;
}

static int c_hlsl_effect_error(void) {
    uint32_t dev = new_device();
    if (!dev) FAIL("no device");
    char first[512] = "";
    uint32_t hr0 = 0;
    for (int k = 0; k < 2; k++) {                       /* the game asks twice with the same text */
        struct effect e = create_effect(dev, "effect_error.fx", 0, 1);
        if (!(e.hr & 0x80000000u)) FAIL("call %d: hr 0x%08X", k + 1, e.hr);
        if (e.effect) FAIL("call %d: an effect 0x%08X", k + 1, e.effect);
        if (!e.errors || e.errors == 0xCDCDCDCDu) FAIL("call %d: no error buffer", k + 1);
        const char *t = errtext(e.errors);
        /* D3DX's file(line,col) form; this compiler gives a syntax error its line alone, file(line) */
        if (!strstr(t, "(3,") && !strstr(t, "(3)")) FAIL("call %d: the errors do not name line 3: %.300s", k + 1, t);
        if (!k) note("line", "%s", strstr(t, "(3,") ? "(3," : "(3)");
        if (!k) snprintf(first, sizeof first, "%s", t), hr0 = e.hr;
        else if (e.hr != hr0 || strcmp(first, t)) FAIL("the second call gave another answer (0x%08X)", e.hr);
        release(e.errors);
    }
    struct effect e = create_effect(dev, "effect_error.fx", 0, 0);      /* no ppErrors: the buffer is released */
    if (e.hr != hr0 || e.effect) FAIL("without ppErrors: 0x%08X, effect 0x%08X", e.hr, e.effect);
    note("hr", "0x%08X", hr0);
    release(dev);
    return 1;
}

static int c_hlsl_fragment_linker(void) {
    uint32_t n, src = input("fragments.fx", &n), out = gmem(8);
    uint32_t hr = lcall("D3DXGatherFragments", 7, src, n, 0u, 0u, 0u, out, out + 4);
    if (hr || !MEM32(out)) FAIL("D3DXGatherFragments 0x%08X: %s", hr, errtext(MEM32(out + 4)));
    uint32_t frags = MEM32(out), dev = new_device(), lo = gmem(4);
    if (!dev) FAIL("no device");
    hr = gcall("D3DXCreateFragmentLinker", 3, dev, 0u, lo);
    uint32_t linker = MEM32(lo);
    if (hr || !linker) FAIL("D3DXCreateFragmentLinker 0x%08X", hr);
    hr = vcall(linker, FL_ADDFRAGMENTS, 1, vcall(frags, BUF_PTR, 0));
    if (hr) FAIL("AddFragments 0x%08X", hr);
    note("fragments", "%u", vcall(linker, FL_NUMFRAGMENTS, 0));
    uint32_t hs = gmem(8);
    MEM32(hs) = vcall(linker, FL_HANDLEBYNAME, 1, gstr("pos"));
    MEM32(hs + 4) = vcall(linker, FL_HANDLEBYNAME, 1, gstr("col"));
    if (!MEM32(hs) || !MEM32(hs + 4)) FAIL("fragment handles 0x%08X 0x%08X", MEM32(hs), MEM32(hs + 4));
    uint32_t vo = gmem(8);
    hr = vcall(linker, FL_LINKVS, 6, gstr("vs_1_1"), 0u, hs, 2u, vo, vo + 4);
    if (hr) FAIL("LinkVertexShader 0x%08X: %s", hr, errtext(MEM32(vo + 4)));
    if (!kind_is(MEM32(vo), D3DK_VSHADER)) FAIL("the linked shader 0x%08X is not our device's", MEM32(vo));
    release(MEM32(vo)), release(MEM32(vo + 4)), release(linker), release(frags), release(MEM32(out + 4)), release(dev);
    return 1;
}

static int c_hlsl_sphere_draw(void) {
    uint32_t dev = new_device(), out = gmem(4);
    if (!dev) FAIL("no device");
    uint32_t hr = gcall("D3DXCreateSphere", 6, dev, f2u(1.0f), 8u, 4u, out, 0u), mesh = MEM32(out);
    if (hr || !mesh) FAIL("D3DXCreateSphere 0x%08X", hr);
    note("mesh", "vertices %u faces %u fvf 0x%08X", vcall(mesh, MESH_NUMVERTICES, 0), vcall(mesh, MESH_NUMFACES, 0),
         vcall(mesh, MESH_FVF, 0));
    if (vcall(mesh, MESH_VB, 1, out) || !kind_is(MEM32(out), D3DK_VB)) FAIL("its vertex buffer 0x%08X is not ours", MEM32(out));
    release(MEM32(out));
    if (vcall(dev, DEV_BEGINSCENE, 0)) FAIL("BeginScene");
    uint32_t n0 = d3d_draw_calls();
    hr = vcall(mesh, MESH_DRAWSUBSET, 1, 0u);
    uint32_t n1 = d3d_draw_calls();
    if (vcall(dev, DEV_ENDSCENE, 0)) FAIL("EndScene");
    note("draws", "%u", n1 - n0);
    if (hr) FAIL("DrawSubset 0x%08X", hr);
    release(mesh), release(dev);
    return 1;
}

/* ---- Task 6: the lifted compiler against the game's own compiled effects ----
 * Both checks read the game's files in memory and print numbers, kind names, offsets, profiles and addresses
 * only: never a name, a message or a byte of shader text from the game. Nothing is written to disk. */

static int g_argc;                                      /* the words after the check's name */
static char *g_argv[64];

/* a file of the game tree, NUL-ended in host memory; NULL when missing */
static uint8_t *host_file(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len + 1);
    *n = (uint32_t)fread(d, 1, (size_t)len, f);
    d[*n] = 0;
    fclose(f);
    return d;
}
/* the line numbers a D3DX message names ("(12):" or "(12,3):"), comma-separated, at most 8, and its error codes
 * (Microsoft's X<number>) */
static void error_lines(uint32_t errors, char *out, size_t n) {
    const char *t = errtext(errors);
    out[0] = 0;
    int k = 0;
    for (const char *p = strchr(t, '('); p && k < 8; p = strchr(p + 1, '(')) {
        char *end;
        unsigned long line = strtoul(p + 1, &end, 10);
        if (end == p + 1 || (*end != ')' && *end != ',')) continue;
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%lu", k++ ? "," : "", line);
    }
    if (!k) snprintf(out, n, "none");
    int codes = 0;
    for (const char *p = strstr(t, "error X"); p && codes < 8; p = strstr(p + 1, "error X")) {
        char *end;
        unsigned long code = strtoul(p + 7, &end, 10);
        if (end == p + 7) continue;
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%lu", codes++ ? ",X" : " codes X", code);
    }
}

/* The comparison of our fx_2_0 binary (a) with the shipped one (b). */
enum { K_ONLY_OURS, K_ONLY_SHIPPED, K_ANNO, K_FLAGS, K_ORDER, K_VALUE, K_BODY, K_SET, K_PVALUE, NKINDS };
static const char *const g_kind[NKINDS] = { "param_only_ours", "param_only_shipped", "annotation_differs",
    "param_flags_differs", "state_order_differs", "state_value_differs", "shader_body_differs",
    "state_set_differs", "param_value_differs" };
#define NO_OFF 0xFFFFFFFFu
struct cmp {
    const struct fx_effect *a, *b;
    uint32_t n[NKINDS], oa[NKINDS], ob[NKINDS];
    uint32_t identical, sha, shb;
    char must[256];                                     /* the first must-hold item that failed */
};
static void finding(struct cmp *c, int k, uint32_t oa, uint32_t ob) {
    if (!c->n[k]++) c->oa[k] = oa, c->ob[k] = ob;
}
static void must(struct cmp *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void must(struct cmp *c, const char *fmt, ...) {
    if (c->must[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->must, sizeof c->must, fmt, ap);
    va_end(ap);
}
/* a byte offset in e's binary of something that points into it */
static uint32_t off_in(const struct fx_effect *e, const void *p) { return p ? (uint32_t)((const uint8_t *)p - e->raw) : NO_OFF; }
static int same_str(const char *x, const char *y) { return (!x && !y) || (x && y && !strcmp(x, y)); }
static uint32_t nchildren(const struct fx_param *p) { return p->elements ? p->elements : p->cls == FXC_STRUCT ? p->nmembers : 0; }

/* a growing byte buffer */
struct bytes { uint8_t *p; size_t n, cap; };
static void put(struct bytes *b, const void *d, size_t n) {
    if (b->n + n > b->cap) b->p = realloc(b->p, b->cap = 2 * (b->n + n) + 64);
    memcpy(b->p + b->n, d, n), b->n += n;
}
/* An expression's meaning (a state's, a selector's or a preshader's tokens): the version, the constant table's
 * entries (name, register set, index, count), CLIT, PRSI and FXLC, and the tokens after the comments. The CTAB's
 * creator and target strings are left out, like any other comment. malloc'd, *len its bytes. */
static uint8_t *expr_meaning(const uint32_t *t, uint32_t n, uint32_t *len) {
    struct bytes b = { 0 };
    put(&b, "", 0);
    uint32_t i = n ? 1 : 0;
    if (n) put(&b, &t[0], 4);
    for (; i < n && (t[i] & 0xFFFFu) == 0xFFFEu;) {
        uint32_t l = (t[i] >> 16) & 0x7FFFu;
        if (l > n - i - 1) break;
        uint32_t cc = l ? t[i + 1] : 0;
        const uint8_t *d = (const uint8_t *)(t + i + 2);
        uint32_t dl = l ? 4 * (l - 1) : 0;
        if (cc == 0x42415443u && dl >= 28) {            /* CTAB: Size, Creator, Version, Constants, ConstantInfo */
            uint32_t nc, ci;
            memcpy(&nc, d + 12, 4), memcpy(&ci, d + 16, 4);
            for (uint32_t k = 0; k < nc && ci <= dl && (dl - ci) / 20 > k; k++) {
                const uint8_t *e = d + ci + 20 * k;
                uint32_t name;
                memcpy(&name, e, 4);
                if (name < dl && memchr(d + name, 0, dl - name)) put(&b, d + name, strlen((const char *)d + name) + 1);
                put(&b, e + 4, 6);                      /* RegisterSet, RegisterIndex, RegisterCount */
            }
        } else if (cc == 0x54494C43u || cc == 0x49535250u || cc == 0x434C5846u) {   /* CLIT, PRSI, FXLC */
            put(&b, &cc, 4);
            put(&b, d, dl);
        }
        i += 1 + l;
    }
    for (; i < n; i++) put(&b, &t[i], 4);
    *len = (uint32_t)b.n;
    return b.p;
}
static int same_expr(const uint8_t *x, uint32_t nx, const uint8_t *y, uint32_t ny) {
    uint32_t la, lb;
    uint8_t *a = expr_meaning((const uint32_t *)x, nx / 4, &la), *b = expr_meaning((const uint32_t *)y, ny / 4, &lb);
    int same = la == lb && !memcmp(a, b, la);
    free(a), free(b);
    return same;
}

/* the shader a state sets (its own, or the parameter it names), or NULL; a selector's is NULL */
static const struct fx_object *state_shader(const struct fx_effect *e, const struct fx_state *s) {
    const struct fx_param *p = s->usage == FX_USAGE_PARAM ? s->ref : s->usage == FX_USAGE_SELECTOR ? NULL : &s->param;
    if (!p || p->cls != FXC_OBJECT || !FX_IS_SHADER(p->type) || p->elements) return NULL;
    return &e->objects[p->object];
}
static uint32_t shader_version(const struct fx_object *o) { uint32_t v = 0; if (o && o->size >= 4) memcpy(&v, o->data, 4); return v; }
static int is_shader_op(uint32_t op) { return op == 146 || op == 147; }  /* VertexShader, PixelShader */

/* two shaders' tokens: without comments (body), and their preshaders (a state value) */
static void cmp_shader(struct cmp *c, const struct fx_object *x, const struct fx_object *y, int pass) {
    uint32_t nx = x->size / 4, ny = y->size / 4;
    uint32_t *sx = malloc(4 * nx + 4), *sy = malloc(4 * ny + 4);
    uint32_t kx = strip((const uint32_t *)x->data, nx, sx), ky = strip((const uint32_t *)y->data, ny, sy);
    if (kx == ky && !memcmp(sx, sy, 4 * kx)) c->identical += pass;
    else finding(c, K_BODY, off_in(c->a, x->data), off_in(c->b, y->data));
    free(sx), free(sy);
    uint32_t lx = 0, ly = 0;
    const uint32_t *px = nx ? fx_comment((const uint32_t *)x->data, nx, 0x53455250u, &lx) : NULL;   /* PRES */
    const uint32_t *py = ny ? fx_comment((const uint32_t *)y->data, ny, 0x53455250u, &ly) : NULL;
    if (!px) lx = 0;
    if (!py) ly = 0;
    if ((lx || ly) && !same_expr((const uint8_t *)px, 4 * lx, (const uint8_t *)py, 4 * ly))
        finding(c, K_VALUE, px ? off_in(c->a, px) : off_in(c->a, x->data), py ? off_in(c->b, py) : off_in(c->b, y->data));
}

static void cmp_state(struct cmp *c, const struct fx_state *x, const struct fx_state *y, int pass) {
    if (is_shader_op(x->op) && x->usage != FX_USAGE_SELECTOR && y->usage != FX_USAGE_SELECTOR) {
        const struct fx_object *ox = state_shader(c->a, x), *oy = state_shader(c->b, y);
        if (ox && oy && ox->size && oy->size) { cmp_shader(c, ox, oy, pass); return; }
    }
    uint32_t oa = x->blob ? off_in(c->a, x->blob) : x->param.voff, ob = y->blob ? off_in(c->b, y->blob) : y->param.voff;
    if (x->usage != y->usage) { finding(c, K_VALUE, oa, ob); return; }
    int same = 1;
    if (x->usage == FX_USAGE_PARAM) same = x->ref && y->ref && same_str(x->ref->name, y->ref->name);
    else if (x->usage == FX_USAGE_SELECTOR) {
        uint32_t lx = 0, ly = 0;
        if (x->bloblen >= 4) memcpy(&lx, x->blob, 4), lx += 4;
        if (y->bloblen >= 4) memcpy(&ly, y->blob, 4), ly += 4;
        same = lx == ly && lx <= x->bloblen && ly <= y->bloblen && !memcmp(x->blob, y->blob, lx)
               && same_expr(x->blob + lx, x->bloblen - lx, y->blob + ly, y->bloblen - ly);
    } else if (x->usage == FX_USAGE_BLOB) same = same_expr(x->blob, x->bloblen, y->blob, y->bloblen);
    else if (x->param.cls == FXC_OBJECT) {
        if (x->param.type == FXT_STRING || FX_IS_SHADER(x->param.type)) {
            const struct fx_object *ox = &c->a->objects[x->param.object], *oy = &c->b->objects[y->param.object];
            same = ox->size == oy->size && (!ox->size || !memcmp(ox->data, oy->data, ox->size));
        }
    } else same = x->param.type == y->param.type && x->param.bytes == y->param.bytes
                  && !memcmp(x->param.data, y->param.data, x->param.bytes);
    if (!same) finding(c, K_VALUE, oa, ob);
}

/* two lists of states (a pass's, or a sampler's): the same operations, in the same order, with the same values;
 * the k-th state of an (operation, index) in one is paired with the k-th in the other */
static uint64_t skey(const struct fx_state *s) { return (uint64_t)s->op << 32 | s->index; }
static void cmp_states(struct cmp *c, const struct fx_state *x, uint32_t nx, const struct fx_state *y, uint32_t ny,
                       uint32_t owner_a, uint32_t owner_b, int pass) {
    uint8_t *used = calloc(ny + 1, 1);
    int in_order = nx == ny, all = nx == ny;
    for (uint32_t i = 0; i < nx; i++) {
        uint32_t j = 0;
        while (j < ny && (used[j] || skey(&y[j]) != skey(&x[i]))) j++;
        if (j == ny) { all = 0; continue; }
        used[j] = 1;
        if (i != j) in_order = 0;
        cmp_state(c, &x[i], &y[j], pass);
    }
    free(used);
    if (!all) finding(c, K_SET, nx ? x[0].at : owner_a, ny ? y[0].at : owner_b);
    else if (!in_order) finding(c, K_ORDER, nx ? x[0].at : owner_a, ny ? y[0].at : owner_b);
}

/* annotations: the same names, types and values */
static int same_value(const struct cmp *c, const struct fx_param *x, const struct fx_param *y);
static int same_annos(const struct cmp *c, const struct fx_param *x, uint32_t nx, const struct fx_param *y, uint32_t ny) {
    if (nx != ny) return 0;
    for (uint32_t i = 0; i < nx; i++) {
        char *sx = fx_signature(&x[i]), *sy = fx_signature(&y[i]);
        int same = !strcmp(sx, sy);
        free(sx), free(sy);
        if (!same || !same_value(c, &x[i], &y[i])) return 0;
    }
    return 1;
}
static void cmp_annos(struct cmp *c, const struct fx_param *x, uint32_t nx, const struct fx_param *y, uint32_t ny,
                      uint32_t owner_a, uint32_t owner_b) {
    if (!same_annos(c, x, nx, y, ny)) finding(c, K_ANNO, nx ? x[0].rec : owner_a, ny ? y[0].rec : owner_b);
}
/* the values of two parameters of one type (numbers and strings; textures hold none) */
static int same_value(const struct cmp *c, const struct fx_param *x, const struct fx_param *y) {
    uint32_t n = nchildren(x);
    if (n) {
        for (uint32_t i = 0; i < n; i++)
            if (!same_value(c, &x->members[i], &y->members[i])) return 0;
        return 1;
    }
    if (x->cls != FXC_OBJECT) return x->bytes == y->bytes && !memcmp(x->data, y->data, x->bytes);
    if (x->type == FXT_STRING) {
        const struct fx_object *ox = &c->a->objects[x->object], *oy = &c->b->objects[y->object];
        return ox->size == oy->size && (!ox->size || !memcmp(ox->data, oy->data, ox->size));
    }
    return 1;
}
/* a common parameter's leaves: numbers and strings (param_value_differs), samplers' states, shaders' tokens */
static void cmp_leaves(struct cmp *c, const struct fx_param *x, const struct fx_param *y) {
    uint32_t n = nchildren(x);
    if (n) {
        for (uint32_t i = 0; i < n; i++) cmp_leaves(c, &x->members[i], &y->members[i]);
        return;
    }
    if (x->cls == FXC_OBJECT && FX_IS_SAMPLER(x->type)) cmp_states(c, x->states, x->nstates, y->states, y->nstates, x->voff, y->voff, 0);
    else if (x->cls == FXC_OBJECT && FX_IS_SHADER(x->type)) {
        const struct fx_object *ox = &c->a->objects[x->object], *oy = &c->b->objects[y->object];
        if (shader_version(ox) != shader_version(oy)) must(c, "shader parameter versions 0x%08X and 0x%08X", shader_version(ox), shader_version(oy));
        else if (ox->size && oy->size) cmp_shader(c, ox, oy, 0);
    } else if (!same_value(c, x, y)) finding(c, K_PVALUE, x->voff, y->voff);
}

static const struct fx_param *param_named(const struct fx_effect *e, const char *name, uint32_t *at) {
    for (uint32_t i = 0; i < e->nparams; i++)
        if (same_str(e->params[i].name, name)) { *at = i; return &e->params[i]; }
    return NULL;
}
/* a pass's shader states, as (operation << 32 | version), sorted */
static int u64cmp(const void *p, const void *q) { uint64_t a = *(const uint64_t *)p, b = *(const uint64_t *)q; return a < b ? -1 : a > b; }
static uint32_t pass_shaders(const struct fx_effect *e, const struct fx_pass *ps, uint64_t *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t s = 0; s < ps->nstates && n < max; s++) {
        const struct fx_state *st = &ps->states[s];
        if (!is_shader_op(st->op)) continue;
        uint32_t v = st->usage == FX_USAGE_SELECTOR ? 0xFFFFFFFFu : shader_version(state_shader(e, st));
        out[n++] = (uint64_t)st->op << 32 | v;
    }
    qsort(out, n, sizeof *out, u64cmp);
    return n;
}

static void compare_effects(struct cmp *c) {
    const struct fx_effect *a = c->a, *b = c->b;
    /* parameters: those in one only, and the common ones' types, order, flags, annotations and values */
    uint32_t last = 0, have_last = 0;
    for (uint32_t i = 0; i < a->nparams; i++) {
        const struct fx_param *x = &a->params[i];
        uint32_t j;
        const struct fx_param *y = param_named(b, x->name, &j);
        if (!y) { finding(c, K_ONLY_OURS, x->rec, NO_OFF); continue; }
        if (have_last && j < last) must(c, "common parameters in another order (offsets 0x%X and 0x%X)", x->rec, y->rec);
        last = j, have_last = 1;
        char *sx = fx_signature(x), *sy = fx_signature(y);
        if (strcmp(sx, sy)) must(c, "a common parameter's type differs (offsets 0x%X and 0x%X)", x->rec, y->rec);
        int same_type = !strcmp(sx, sy);
        free(sx), free(sy);
        if (!same_type) continue;
        if (x->flags != y->flags) finding(c, K_FLAGS, x->rec + 8, y->rec + 8);
        cmp_annos(c, x->annos, x->nannos, y->annos, y->nannos, x->rec, y->rec);
        cmp_leaves(c, x, y);
    }
    for (uint32_t j = 0; j < b->nparams; j++) {
        uint32_t i;
        if (!param_named(a, b->params[j].name, &i)) finding(c, K_ONLY_SHIPPED, NO_OFF, b->params[j].rec);
    }
    /* techniques and passes: the same names in the same order; per pass the same shader slots and versions */
    if (a->ntechs != b->ntechs) must(c, "%u and %u techniques", a->ntechs, b->ntechs);
    for (uint32_t t = 0; t < a->ntechs && t < b->ntechs; t++) {
        const struct fx_technique *x = &a->techs[t], *y = &b->techs[t];
        if (!same_str(x->name, y->name)) must(c, "technique %u's names differ (offsets 0x%X and 0x%X)", t, off_in(a, x->name), off_in(b, y->name));
        if (x->npasses != y->npasses) must(c, "technique %u: %u and %u passes", t, x->npasses, y->npasses);
        cmp_annos(c, x->annos, x->nannos, y->annos, y->nannos, off_in(a, x->name), off_in(b, y->name));
        for (uint32_t p = 0; p < x->npasses && p < y->npasses; p++) {
            const struct fx_pass *px = &x->passes[p], *py = &y->passes[p];
            if (!same_str(px->name, py->name)) must(c, "technique %u pass %u's names differ", t, p);
            uint64_t sx[32], sy[32];
            uint32_t nx = pass_shaders(a, px, sx, 32), ny = pass_shaders(b, py, sy, 32);
            if (nx != ny || memcmp(sx, sy, 8u * nx))
                must(c, "technique %u pass %u: shader slots differ (%u and %u; first 0x%llX and 0x%llX)", t, p, nx, ny,
                     nx ? (unsigned long long)sx[0] : 0ull, ny ? (unsigned long long)sy[0] : 0ull);
            cmp_annos(c, px->annos, px->nannos, py->annos, py->nannos, off_in(a, px->name), off_in(b, py->name));
            cmp_states(c, px->states, px->nstates, py->states, py->nstates, off_in(a, px->name), off_in(b, py->name), 1);
        }
    }
}
static void count_pass_shaders(const struct fx_effect *e, uint32_t *passes, uint32_t *shaders) {
    *passes = *shaders = 0;
    for (uint32_t t = 0; t < e->ntechs; t++)
        for (uint32_t p = 0; p < e->techs[t].npasses; p++, (*passes)++)
            for (uint32_t s = 0; s < e->techs[t].passes[p].nstates; s++) {
                const struct fx_state *st = &e->techs[t].passes[p].states[s];
                const struct fx_object *o = is_shader_op(st->op) ? state_shader(e, st) : NULL;
                *shaders += o && o->size >= 4;
            }
}

/* ID3DXEffectCompiler::CompileEffect */
enum { EC_COMPILEEFFECT = 59 };
#define SDV_SOURCE_AT 372u
static int sdv_one(const char *game, const char *name) {
    char path[2048];
    snprintf(path, sizeof path, "%s/Data/effects/%s.sdv", game, name);
    uint32_t n;
    uint8_t *f = host_file(path, &n);
    if (!f) { printf("sdv %s: no file\n", name); return 0; }
    static const uint8_t magic[4] = { 0x01, 0x09, 0xFF, 0xFE };
    uint32_t fx = 0;
    while (fx + 4 <= n && memcmp(f + fx, magic, 4)) fx++;
    uint32_t srclen = n > SDV_SOURCE_AT ? (uint32_t)strnlen((const char *)f + SDV_SOURCE_AT, n - SDV_SOURCE_AT) : 0;
    if (fx + 4 > n || !srclen || fx < SDV_SOURCE_AT + srclen) {
        printf("sdv %s: source %u bytes at %u, fx_2_0 at %u of %u\n", name, srclen, SDV_SOURCE_AT, fx, n);
        free(f);
        return 0;
    }
    /* our compile: D3DXCreateEffectCompiler(text, no defines, no include, flags 0), CompileEffect(0) */
    uint32_t src = gmem(srclen + 1), out = gmem(16);
    memcpy(hle_ptr(src), f + SDV_SOURCE_AT, srclen);
    uint32_t hr = lcall("D3DXCreateEffectCompiler", 7, src, srclen, 0u, 0u, 0u, out, out + 4);
    uint32_t compiler = MEM32(out), buf = 0;
    if (!(hr & 0x80000000u)) {
        release(MEM32(out + 4));
        MEM32(out + 4) = 0;
        hr = vcall(compiler, EC_COMPILEEFFECT, 3, 0u, out + 8, out + 4);
        buf = MEM32(out + 8);
    }
    release(compiler);
    if ((hr & 0x80000000u) || !buf) {
        char lines[128];
        error_lines(MEM32(out + 4), lines, sizeof lines);
        printf("sdv %s: compile 0x%08X, error lines %s\n", name, hr, lines);
        release(MEM32(out + 4)), release(buf);
        free(f);
        return 0;
    }
    release(MEM32(out + 4));
    struct fx_effect a = { 0 }, b = { 0 };
    char ea[256], eb[256];
    int pa = fx_parse(hle_ptr(vcall(buf, BUF_PTR, 0)), vcall(buf, BUF_SIZE, 0), &a, ea, sizeof ea);
    int pb = fx_parse(f + fx, n - fx, &b, eb, sizeof eb);
    release(buf);
    free(f);
    if (pa || pb) {                                     /* (a message may quote a name: cut it at the quote) */
        ea[strcspn(ea, "'")] = 0, eb[strcspn(eb, "'")] = 0;
        printf("sdv %s: compile 0x%08X, fx_parse rejects %s: %s\n", name, hr, pa ? "ours" : "the shipped binary", pa ? ea : eb);
        fx_free(&a), fx_free(&b);
        return 0;
    }
    struct cmp c = { .a = &a, .b = &b };
    compare_effects(&c);
    uint32_t passa, passb, common = 0, j;
    count_pass_shaders(&a, &passa, &c.sha), count_pass_shaders(&b, &passb, &c.shb);
    for (uint32_t i = 0; i < a.nparams; i++) common += param_named(&b, a.params[i].name, &j) != NULL;
    char list[1024] = "";
    for (int k = 0; k < NKINDS; k++) {
        if (!c.n[k]) continue;
        size_t l = strlen(list);
        char oa[16] = "-", ob[16] = "-";
        if (c.oa[k] != NO_OFF) snprintf(oa, sizeof oa, "0x%X", c.oa[k]);
        if (c.ob[k] != NO_OFF) snprintf(ob, sizeof ob, "0x%X", c.ob[k]);
        snprintf(list + l, sizeof list - l, "%s%s=%u@%s/%s", l ? " " : "", g_kind[k], c.n[k], oa, ob);
    }
    printf("sdv %s: compile 0x%08X, params %u/%u common %u, techniques %u/%u, passes %u/%u, shaders %u/%u, "
           "identical shaders %u, findings %s\n", name, hr, a.nparams, b.nparams, common, a.ntechs, b.ntechs, passa,
           passb, c.sha, c.shb, c.identical, list[0] ? list : "none");
    if (c.must[0]) printf("sdv %s: must-hold failed: %s\n", name, c.must);
    fflush(stdout);
    fx_free(&a), fx_free(&b);
    return !c.must[0];
}
static int c_sdv_oracle(void) {
    if (g_argc < 2) FAIL("usage: sdv_oracle <game dir> <name>...");
    int bad = 0;
    for (int i = 1; i < g_argc; i++) bad += !sdv_one(g_argv[0], g_argv[i]);
    if (bad) FAIL("%d of %d files failed", bad, g_argc - 1);
    return 1;
}

/* D3DXCompileShader as the game's wrappers call it (sub_AB3130, sub_AB31F0): the decoded file's bytes, its length,
 * the defines (none at both sites), no include, the entry and profile the caller passes, D3DXSHADER_DEBUG (1),
 * and no constant table */
#define GAME_COMPILE_FLAGS 1u
static int fxsrc_one(const char *game, const char *item) {
    char stem[256], profile[32];
    unsigned entry = 0, defines = 0;
    int k = sscanf(item, "%255[^:]:%x:%31[^:]:%x", stem, &entry, profile, &defines);
    if (k < 3) { printf("fxsrc %s: not <stem>:<entry va>:<profile>[:<defines va>]\n", item); return 0; }
    char path[2048];
    snprintf(path, sizeof path, "%s/Data/EncryptedShaders/%s.fx.caesar3", game, stem);
    uint32_t n;
    uint8_t *f = host_file(path, &n);
    if (!f) { printf("fxsrc %s: no file\n", stem); return 0; }
    if (entry < 0x400000u || entry >= 0x2451210u || !memchr(hle_ptr(entry), 0, 256)) {
        printf("fxsrc %s 0x%08X: the entry is no string in white.exe\n", stem, entry);
        free(f);
        return 0;
    }
    uint32_t src = gmem(n + 1), out = gmem(12);
    for (uint32_t i = 0; i < n; i++) MEM8(src + i) = (uint8_t)(f[i] - 3);
    free(f);
    uint32_t hr = lcall("D3DXCompileShader", 10, src, n, (uint32_t)defines, 0u, (uint32_t)entry, gstr(profile),
                        GAME_COMPILE_FLAGS, out, out + 4, 0u);
    struct compiled c = { .hr = hr, .shader = MEM32(out), .errors = MEM32(out + 4) };
    take_tokens(&c);
    int ok = 0;
    if (!(hr & 0x80000000u) && c.ntok) {
        struct sm_shader s;
        char err[256];
        ok = !sm_decode(c.tok, c.ntok, &s, err, sizeof err);
        if (ok) sm_free(&s);
    }
    printf("fxsrc %s 0x%08X %s: %u bytes, compile 0x%08X, version 0x%08X, %u tokens, decode %s\n", stem, entry, profile,
           n, hr, c.ntok ? c.tok[0] : 0, c.ntok, ok ? "ok" : "FAIL");
    if (hr & 0x80000000u) {
        char lines[128];
        error_lines(c.errors, lines, sizeof lines);
        printf("fxsrc %s 0x%08X %s: error lines %s\n", stem, entry, profile, lines);
    }
    fflush(stdout);
    drop(&c);
    return ok;
}
static int c_fxsrc_compile(void) {
    if (g_argc < 2) FAIL("usage: fxsrc_compile <game dir> <stem>:<entry va>:<profile>[:<defines va>]...");
    int bad = 0;
    for (int i = 1; i < g_argc; i++) bad += !fxsrc_one(g_argv[0], g_argv[i]);
    if (bad) FAIL("%d of %d compiles failed", bad, g_argc - 1);
    return 1;
}

static const struct { const char *name; int (*fn)(void); } g_checks[] = {
    { "d3dx_load", c_d3dx_load }, { "d3dx_data_import", c_d3dx_data_import },
    { "d3dx_module_name", c_d3dx_module_name }, { "d3dx_validator", c_d3dx_validator }, { "d3dx_cpu", c_d3dx_cpu },
    { "hlsl_compile_ps", c_hlsl_compile_ps }, { "hlsl_compile_vs", c_hlsl_compile_vs },
    { "hlsl_compile_error", c_hlsl_compile_error }, { "hlsl_asm_matches_native", c_hlsl_asm_matches_native },
    { "hlsl_constant_table", c_hlsl_constant_table }, { "hlsl_two_threads", c_hlsl_two_threads },
    { "hlsl_from_file", c_hlsl_from_file },
    { "hlsl_effect", c_hlsl_effect }, { "hlsl_effect_stateblock", c_hlsl_effect_stateblock },
    { "hlsl_effect_pool", c_hlsl_effect_pool }, { "hlsl_effect_error", c_hlsl_effect_error },
    { "hlsl_fragment_linker", c_hlsl_fragment_linker }, { "hlsl_sphere_draw", c_hlsl_sphere_draw },
    { "sdv_oracle", c_sdv_oracle }, { "fxsrc_compile", c_fxsrc_compile },
};

int bw2_d3dx_check(const char *dir) {
    g_dir = dir;
    mach_enter();
    int failed = 0;
    static char line[16384];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        g_argc = 0;                                     /* "<check> [arguments]", split at spaces */
        char *save = NULL;
        strtok_r(line, " ", &save);
        for (char *w; g_argc < 64 && (w = strtok_r(NULL, " ", &save));) g_argv[g_argc++] = w;
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
