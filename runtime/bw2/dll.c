/*
 * dll.c - DLLs whose code is lifted with the game (spec 6.3: the game loads
 * PlugIns\ScriptLibraryR.dll, lifted at its base 0x10000000).
 *
 * LoadLibraryA of a listed DLL (win32hle's hle_lifted_dll_hook) maps it at its
 * preferred base without relocation, since the lift assumed that base, and
 * stops loudly if the range is taken. It binds the DLL's imports as the main
 * image's are bound, sets up its static TLS for threads made from now on, and
 * runs its lifted entry once as DllMain(hmod, DLL_PROCESS_ATTACH, 0). A FALSE
 * fails the load with ERROR_DLL_INIT_FAILED. win32hle's GetProcAddress answers
 * its exports from its export table, and a second LoadLibraryA only counts.
 *
 * The engine loads one lifted DLL itself (plan 9): the disc's d3dx9_25.dll,
 * rebased to 0x08000000, through bw2_lifted_dll_open with the same steps. It is
 * registered under a module name of its own, so the game's GetModuleHandleA and
 * GetProcAddress never find it in place of the native D3DX.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "win32hle.h"
#include "pe_loader.h"
#include "bw2.h"

#define ERROR_MOD_NOT_FOUND     126u
#define ERROR_DLL_INIT_FAILED  1114u
#define MAX_LIFTED 8
/* win32hle's "anywhere" allocations start at 0x10000000, ScriptLibraryR's
 * base: each listed DLL's range is held from start-up until it is loaded. */
#define HOLD 0x01000000u

static struct { char name[64]; uint32_t base, held; } g_lifted[MAX_LIFTED];
static int g_nlifted;
/* The engine's own lifted DLLs, held from start-up too (plan 9: the lifted d3dx9_25.dll). */
static struct { uint32_t base, held, hmod; char module[64]; } g_engine[] = { { 0x08000000u, 0, 0, "" } };
#define N_ENGINE (int)(sizeof g_engine / sizeof *g_engine)

void bw2_lifted_dll_add(const char *name, uint32_t base) {
    for (int i = 0; i < g_nlifted; i++)
        if (!strcasecmp(g_lifted[i].name, name)) { g_lifted[i].base = base; return; }
    if (g_nlifted == MAX_LIFTED) hle_fatal("more than %d lifted DLLs", MAX_LIFTED);
    snprintf(g_lifted[g_nlifted].name, sizeof g_lifted[g_nlifted].name, "%s", name);
    for (char *c = g_lifted[g_nlifted].name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    g_lifted[g_nlifted++].base = base;
}

/* Data imports: an IAT slot that must hold the address of a variable, not of
 * code. A shim stub address there (the never-mapped shim range) would fault on
 * the first read with no name, so each known one is answered here with a
 * dword of guest memory, made once, or stops the load by name. msvcrt's
 * documented data exports are listed; only _adjust_fdiv has an answer (the
 * lifted D3DX's CRT init reads it on every DllMain call, plan 9). */
static const struct { const char *name; int answered; uint32_t value; } g_data_imports[] = {
    { "_adjust_fdiv", 1, 0 },        /* 0: no Pentium FDIV-flaw workaround */
    { "_iob", 0, 0 }, { "_pctype", 0, 0 }, { "_pwctype", 0, 0 }, { "__mb_cur_max", 0, 0 }, { "_mbctype", 0, 0 },
    { "_mbcasemap", 0, 0 }, { "_environ", 0, 0 }, { "_wenviron", 0, 0 }, { "__initenv", 0, 0 },
    { "__winitenv", 0, 0 }, { "__argc", 0, 0 }, { "__argv", 0, 0 }, { "__wargv", 0, 0 }, { "_acmdln", 0, 0 },
    { "_wcmdln", 0, 0 }, { "_pgmptr", 0, 0 }, { "_wpgmptr", 0, 0 }, { "_fmode", 0, 0 }, { "_commode", 0, 0 },
    { "_HUGE", 0, 0 }, { "_osver", 0, 0 }, { "_winver", 0, 0 }, { "_winmajor", 0, 0 }, { "_winminor", 0, 0 },
    { "_sys_errlist", 0, 0 }, { "_sys_nerr", 0, 0 }, { "_timezone", 0, 0 }, { "_daylight", 0, 0 },
    { "_dstbias", 0, 0 }, { "_tzname", 0, 0 }, { "__badioinfo", 0, 0 }, { "__pioinfo", 0, 0 },
    { "_aexit_rtn", 0, 0 }, { "_crtAssertBusy", 0, 0 }, { "_crtBreakAlloc", 0, 0 }, { "_crtDbgFlag", 0, 0 },
};
#define N_DATA_IMPORTS (sizeof g_data_imports / sizeof *g_data_imports)
static uint32_t g_data_va[N_DATA_IMPORTS];

/* The resolver every lifted DLL's imports are bound with: a data import's
 * variable, else hle_resolve_or_stub (a shim, or a stub that stops loudly by
 * name when called). Binds run under the machine lock. */
uint32_t bw2_bind_resolve(const char *name) {
    for (size_t i = 0; i < N_DATA_IMPORTS; i++) {
        if (strcmp(g_data_imports[i].name, name)) continue;
        if (!g_data_imports[i].answered) {
            fprintf(stderr, "[bw2] FATAL: lifted DLL imports the data %s, which nothing answers\n", name);
            fflush(stderr);
            abort();
        }
        if (!g_data_va[i]) {
            void *p = hle_alloc(4);
            if (!p) hle_fatal("no guest memory for the data import %s", name);
            *(uint32_t *)p = g_data_imports[i].value;
            g_data_va[i] = hle_va(p);
        }
        return g_data_va[i];
    }
    return hle_resolve_or_stub(name);
}

/* The last bind's imports that nothing answers (each bound to a stub that stops
 * when called, by name), for the loader line and the checks. */
#define MAX_UNANSWERED 256
static const char *g_unanswered[MAX_UNANSWERED];
static int g_nunanswered;
static uint32_t bind_counted(const char *name) {
    uint32_t va = bw2_bind_resolve(name);
    if (hle_is_stub(va)) {
        if (g_nunanswered < MAX_UNANSWERED) g_unanswered[g_nunanswered] = name;
        g_nunanswered++;
    }
    return va;
}
int bw2_lifted_dll_unanswered(const char ***names) {
    if (names) *names = g_unanswered;
    return g_nunanswered < MAX_UNANSWERED ? g_nunanswered : MAX_UNANSWERED;
}

/* Release every hold (listed or engine DLL) that overlaps [base, base + HOLD). */
static void release_holds(uint32_t base) {
    for (int j = 0; j < g_nlifted; j++)              /* entries may share a base */
        if (g_lifted[j].held && g_lifted[j].base < base + HOLD && base < g_lifted[j].base + HOLD)
            hle_vm_release(g_lifted[j].base, HOLD), g_lifted[j].held = 0;
    for (int j = 0; j < N_ENGINE; j++)
        if (g_engine[j].held && g_engine[j].base < base + HOLD && base < g_engine[j].base + HOLD)
            hle_vm_release(g_engine[j].base, HOLD), g_engine[j].held = 0;
}

/* Map host at base, register it as guest, bind, set up TLS and EH, run DllMain. 1 with *hmod, or -1
 * (DllMain said FALSE). name is what the loader line calls it. */
static int map_and_start(const char *name, const char *host, const char *guest, uint32_t base, uint32_t *hmod) {
    pe_image img;
    release_holds(base);
    if (recomp_pe_map(host, &img) != 0)
        hle_fatal("lifted DLL %s: cannot map it at its base 0x%08X (the range is taken)", name, base);
    if (img.base != base)
        hle_fatal("lifted DLL %s: mapped at 0x%08X, but it was lifted for 0x%08X", name, img.base, base);
    hle_module_add(guest, img.base, img.resource_rva, img.resource_size, 0);
    g_nunanswered = 0;
    recomp_pe_bind(&img, bind_counted);
    bw2_tls_add_module(img.base);
    bw2_eh_add_image(img.base, img.span);
    fprintf(stderr, "[bw2] %s: lifted DLL at 0x%08X, %d imports unanswered\n", name, img.base, g_nunanswered);
    uint32_t args[3] = { img.base, 1u /* DLL_PROCESS_ATTACH */, 0 };
    if (img.entry != img.base && !hle_call_guest(img.entry, 3, args)) {
        fprintf(stderr, "[bw2] %s: DllMain returned FALSE\n", name);
        return -1;
    }
    *hmod = img.base;
    return 1;
}

static int load(const char *base_name, const char *host, uint32_t *hmod) {
    if (bw2_later_dll(base_name)) bw2_later_load(base_name);
    int i = 0;
    while (i < g_nlifted && strcmp(g_lifted[i].name, base_name)) i++;
    if (i == g_nlifted) return 0;
    if (!host) { hle_set_last_error(ERROR_MOD_NOT_FOUND); return -1; }
    char guest[600];
    hle_guest_path(host, guest, sizeof guest);
    if (map_and_start(base_name, host, guest, g_lifted[i].base, hmod) < 0) {
        hle_set_last_error(ERROR_DLL_INIT_FAILED);
        return -1;
    }
    return 1;
}

/* A lifted DLL the engine itself needs: mapped at base (which must be an engine
 * DLL's), registered as module_name in the host file's guest folder, bound,
 * started as the game's LoadLibraryA would. Its module handle; the same handle
 * on a second call; 0 when DllMain said FALSE. Not added to the game's
 * LoadLibraryA list. Call under the machine lock. */
uint32_t bw2_lifted_dll_open(const char *module_name, const char *host_path, uint32_t base) {
    int i = 0;
    while (i < N_ENGINE && g_engine[i].base != base) i++;
    if (i == N_ENGINE) hle_fatal("lifted DLL %s: 0x%08X is no engine DLL's base", module_name, base);
    if (g_engine[i].hmod) return g_engine[i].hmod;
    char guest[600];
    hle_guest_path(host_path, guest, sizeof guest);
    char *bs = strrchr(guest, '\\');
    snprintf(bs ? bs + 1 : guest, sizeof guest - (size_t)(bs ? bs + 1 - guest : 0), "%s", module_name);
    const char *file = strrchr(host_path, '/');
    uint32_t h = 0;
    if (map_and_start(file ? file + 1 : host_path, host_path, guest, base, &h) < 0) return 0;
    snprintf(g_engine[i].module, sizeof g_engine[i].module, "%s", module_name);
    return g_engine[i].hmod = h;
}

/* Hold each listed DLL's range, except one that overlaps the main image (its
 * load then fails, naming it). Call after hle_mem_init, before any allocation. */
void bw2_dll_hold(uint32_t main_base, uint32_t main_span) {
    for (int i = 0; i < g_nlifted; i++) {
        uint32_t b = g_lifted[i].base;
        if (b < main_base + main_span && main_base < b + HOLD) continue;
        g_lifted[i].held = hle_vm_reserve(b, HOLD) == b;
    }
    for (int i = 0; i < N_ENGINE; i++) {
        uint32_t b = g_engine[i].base;
        if (b < main_base + main_span && main_base < b + HOLD) continue;
        g_engine[i].held = hle_vm_reserve(b, HOLD) == b;
    }
}

void bw2_dll_init(void) {
    hle_lifted_dll_hook = load;
    bw2_lifted_dll_add("scriptlibraryr.dll", 0x10000000u);
}
