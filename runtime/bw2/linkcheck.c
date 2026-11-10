/*
 * linkcheck.c - the smallest host that lifted BW2 code links against.
 *
 * It defines what recomp_types.h and the generated tables expect, answers
 * lookups from the dispatch table, and turns every failure into a loud abort
 * that names the guest address. It never runs lifted code: its only job is to
 * prove that every shard compiled and every symbol resolves. Plan 3 replaces it
 * with pcrecomp's win32hle host.
 */
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "recomp_funcs.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double   g_st[8];
int      g_fp_top;
uint16_t g_fpu_cw = 0x027F;
uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
ptrdiff_t g_mem_base;
uint32_t g_fs_base, g_gs_base, g_es_base;
uint64_t g_mm[8];
xmm_t    g_xmm[8];
uint32_t g_mxcsr = 0x1F80;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;

static void die(const char *what, uint32_t va, uint32_t from) {
    fprintf(stderr, "linkcheck: %s at 0x%08X (from 0x%08X)\n", what, va, from);
    abort();
}

void recomp_trap(uint32_t va, const char *mnem) { fprintf(stderr, "trap: %s\n", mnem); die("unimplemented instruction", va, g_cur_func); }
void recomp_unresolved_icall(uint32_t va, uint32_t from) { die("unresolved indirect call", va, from); }
void recomp_unresolved_call(uint32_t va) { die("unresolved direct call", va, g_cur_func); }
void recomp_dump_trace(const char *why) { (void)why; }

void     recomp_int(int n) { die("int", (uint32_t)n, g_cur_func); }
uint32_t recomp_in32(uint32_t port) { die("in", port, g_cur_func); return 0; }
void     recomp_out8(uint32_t port, uint8_t v) { (void)v; die("out", port, g_cur_func); }
void     recomp_out16(uint32_t port, uint16_t v) { (void)v; die("out", port, g_cur_func); }
void     recomp_out32(uint32_t port, uint32_t v) { (void)v; die("out", port, g_cur_func); }
void     recomp_ins(uint32_t port, uint32_t va, int size, uint32_t count, int dir) { (void)va; (void)size; (void)count; (void)dir; die("ins", port, g_cur_func); }
void     recomp_outs(uint32_t port, uint32_t va, int size, uint32_t count, int dir) { (void)va; (void)size; (void)count; (void)dir; die("outs", port, g_cur_func); }
void     recomp_cli(void) { die("cli", 0, g_cur_func); }
void     recomp_sti(void) { die("sti", 0, g_cur_func); }
void     recomp_hlt(void) { die("hlt", 0, g_cur_func); }
void     recomp_set_seg(int sreg, uint32_t sel) { (void)sreg; die("segment load", sel, g_cur_func); }
void     recomp_eh_push(recomp_ehframe_t *f) { (void)f; die("SEH push", 0, g_cur_func); }
void     recomp_eh_pop(recomp_ehframe_t *f) { (void)f; die("SEH pop", 0, g_cur_func); }
void     recomp_eh_resume(recomp_ehframe_t *f, uint32_t resume) { (void)f; die("SEH resume", resume, g_cur_func); }

recomp_func_t recomp_lookup(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t a = recomp_dispatch_table[mid].address;
        if (a == va) return recomp_dispatch_table[mid].func;
        if (a < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_import(uint32_t va) { (void)va; return NULL; }

int main(void) {
    int ok = recomp_lookup(bw2_entry_va) != NULL;
    printf("linkcheck: %u functions, entry 0x%08X -> %s\n", recomp_dispatch_count, bw2_entry_va,
           ok ? "found" : "missing");
    return ok ? 0 : 1;
}
