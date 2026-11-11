/*
 * fail.c - what lifted code calls when it reaches something the port does not
 * do: an instruction the lifter trapped, a call to an address nothing lifted,
 * port I/O, privileged instructions, segment loads. Each one stops the run and
 * names the guest address and the function it was in (spec anti-goal: never a
 * silent eax = 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"

void recomp_unresolved_call(uint32_t va);

static void die(const char *what, uint32_t va, uint32_t from) {
    fprintf(stderr, "[bw2] FATAL: %s at 0x%08X (in sub_%08X)\n", what, va, from);
    fflush(stderr);
    abort();
}

void recomp_trap(uint32_t va, const char *mnem) {
    fprintf(stderr, "[bw2] trap: unimplemented instruction %s\n", mnem);
    die("unimplemented instruction", va, g_cur_func);
}
void recomp_unresolved_icall(uint32_t va, uint32_t from) { die("unresolved indirect call", va, from); }
void recomp_unresolved_call(uint32_t va) { die("unresolved direct call", va, g_cur_func); }
void recomp_dump_trace(const char *why) { (void)why; }

void     recomp_int(int n) { die("int", (uint32_t)n, g_cur_func); }
uint32_t recomp_in32(uint32_t port) { die("in", port, g_cur_func); return 0; }
void     recomp_out8(uint32_t port, uint8_t v) { (void)v; die("out", port, g_cur_func); }
void     recomp_out16(uint32_t port, uint16_t v) { (void)v; die("out", port, g_cur_func); }
void     recomp_out32(uint32_t port, uint32_t v) { (void)v; die("out", port, g_cur_func); }
void     recomp_ins(uint32_t port, uint32_t va, int size, uint32_t count, int dir) {
    (void)va; (void)size; (void)count; (void)dir; die("ins", port, g_cur_func);
}
void     recomp_outs(uint32_t port, uint32_t va, int size, uint32_t count, int dir) {
    (void)va; (void)size; (void)count; (void)dir; die("outs", port, g_cur_func);
}
void     recomp_cli(void) { die("cli", 0, g_cur_func); }
void     recomp_sti(void) { die("sti", 0, g_cur_func); }
void     recomp_hlt(void) { die("hlt", 0, g_cur_func); }
void     recomp_set_seg(int sreg, uint32_t sel) { (void)sreg; die("segment load", sel, g_cur_func); }
