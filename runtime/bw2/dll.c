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
 */
#include <stdio.h>
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

void bw2_lifted_dll_add(const char *name, uint32_t base) {
    for (int i = 0; i < g_nlifted; i++)
        if (!strcasecmp(g_lifted[i].name, name)) { g_lifted[i].base = base; return; }
    if (g_nlifted == MAX_LIFTED) hle_fatal("more than %d lifted DLLs", MAX_LIFTED);
    snprintf(g_lifted[g_nlifted].name, sizeof g_lifted[g_nlifted].name, "%s", name);
    for (char *c = g_lifted[g_nlifted].name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    g_lifted[g_nlifted++].base = base;
}

static int load(const char *base_name, const char *host, uint32_t *hmod) {
    int i = 0;
    while (i < g_nlifted && strcmp(g_lifted[i].name, base_name)) i++;
    if (i == g_nlifted) return 0;
    if (!host) { hle_set_last_error(ERROR_MOD_NOT_FOUND); return -1; }
    pe_image img;
    for (int j = 0; j < g_nlifted; j++)              /* any hold on this range (entries may share a base) */
        if (g_lifted[j].held && g_lifted[j].base < g_lifted[i].base + HOLD && g_lifted[i].base < g_lifted[j].base + HOLD)
            hle_vm_release(g_lifted[j].base, HOLD), g_lifted[j].held = 0;
    if (recomp_pe_map(host, &img) != 0)
        hle_fatal("lifted DLL %s: cannot map it at its base 0x%08X (the range is taken)", base_name, g_lifted[i].base);
    if (img.base != g_lifted[i].base)
        hle_fatal("lifted DLL %s: mapped at 0x%08X, but it was lifted for 0x%08X", base_name, img.base, g_lifted[i].base);
    char guest[600];
    hle_guest_path(host, guest, sizeof guest);
    hle_module_add(guest, img.base, img.resource_rva, img.resource_size, 0);
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
    bw2_tls_add_module(img.base);
    bw2_eh_add_image(img.base, img.span);
    fprintf(stderr, "[bw2] %s: lifted DLL at 0x%08X, %d imports unanswered\n", base_name, img.base, unresolved);
    uint32_t args[3] = { img.base, 1u /* DLL_PROCESS_ATTACH */, 0 };
    if (img.entry != img.base && !hle_call_guest(img.entry, 3, args)) {
        fprintf(stderr, "[bw2] %s: DllMain returned FALSE\n", base_name);
        hle_set_last_error(ERROR_DLL_INIT_FAILED);
        return -1;
    }
    *hmod = img.base;
    return 1;
}

/* Hold each listed DLL's range, except one that overlaps the main image (its
 * load then fails, naming it). Call after hle_mem_init, before any allocation. */
void bw2_dll_hold(uint32_t main_base, uint32_t main_span) {
    for (int i = 0; i < g_nlifted; i++) {
        uint32_t b = g_lifted[i].base;
        if (b < main_base + main_span && main_base < b + HOLD) continue;
        g_lifted[i].held = hle_vm_reserve(b, HOLD) == b;
    }
}

void bw2_dll_init(void) {
    hle_lifted_dll_hook = load;
    bw2_lifted_dll_add("scriptlibraryr.dll", 0x10000000u);
}
