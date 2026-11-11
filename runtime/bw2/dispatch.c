/*
 * dispatch.c - recomp_lookup in O(1): an open-addressing hash over the
 * generated dispatch table (spec 7.1: indirect dispatch must not be a binary
 * search). Built once, before any guest code runs; read-only after that.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "bw2.h"

static uint32_t *g_keys;          /* guest VA, 0 = empty */
static recomp_func_t *g_vals;
static uint32_t g_mask;

static inline uint32_t slot(uint32_t va) { return (va * 0x9E3779B1u) >> 7; }

static void build_table(void) {
    uint32_t n = recomp_dispatch_count, size = 16;
    while (size < 2 * n) size <<= 1;
    g_keys = calloc(size, sizeof *g_keys);
    g_vals = calloc(size, sizeof *g_vals);
    if (!g_keys || !g_vals) { fprintf(stderr, "[bw2] no memory for the dispatch table\n"); abort(); }
    g_mask = size - 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t va = recomp_dispatch_table[i].address, h = slot(va) & g_mask;
        while (g_keys[h] && g_keys[h] != va) h = (h + 1) & g_mask;
        g_keys[h] = va;
        g_vals[h] = recomp_dispatch_table[i].func;
    }
}

void bw2_dispatch_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, build_table);
}

recomp_func_t recomp_lookup(uint32_t va) {
    if (!g_keys) bw2_dispatch_init();
    if (!va) return NULL;
    for (uint32_t h = slot(va) & g_mask; g_keys[h]; h = (h + 1) & g_mask)
        if (g_keys[h] == va) return g_vals[h];
    return NULL;
}

int bw2_dispatch_selftest(void) {
    uint32_t n = recomp_dispatch_count, bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t va = recomp_dispatch_table[i].address;
        if (recomp_lookup(va) != recomp_dispatch_table[i].func) bad++;
        int next_is_fn = i + 1 < n && recomp_dispatch_table[i + 1].address == va + 1;
        if (!next_is_fn && recomp_lookup(va + 1)) bad++;
    }
    if (bad) { printf("dispatch FAILED %u\n", bad); return 1; }
    printf("dispatch ok %u\n", n);
    return 0;
}
