/*
 * mtl_buffer.m - GPU-visible guest memory for vertex and index buffers (plan 4
 * Task 5). Arenas of guest memory, page-aligned, are each wrapped once as an
 * MTLBuffer with no copy: the GPU reads what the guest wrote. Blocks are
 * carved from them; a block the GPU may still read is freed only after the
 * command buffer that last used it has finished.
 */
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "mtl.h"
#include "mtl_internal.h"

#define ARENA (64u << 20)
#define ALIGN 256u

struct extent { uint32_t off, size; };
struct arena {
    uint32_t va, size;
    void *buf;                          /* id<MTLBuffer>, retained */
    struct extent *free;                /* sorted by offset, coalesced */
    uint32_t nfree, capfree;
};
static struct arena *g_arenas;
static uint32_t g_narenas;
/* live blocks: va -> (arena, size) */
struct block { uint32_t va, size, arena; };
static struct block *g_blocks;
static uint32_t g_nblocks, g_capblocks;
/* frees waiting for the GPU */
struct pending { uint32_t va; void *ctx; uint64_t serial; };
static struct pending *g_pending;
static uint32_t g_npending, g_cappending;

static void *grow(void *p, uint32_t *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 64;
    p = realloc(p, elem * *cap);
    if (!p) abort();
    return p;
}

static uint32_t new_arena(uint32_t size) {
    uint32_t va = hle_vm_reserve(0, size);
    if (!va) { fprintf(stderr, "[bw2] FATAL: d3d9: no guest memory for a %u-byte buffer arena\n", size); abort(); }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:hle_ptr(va) length:size options:MTLResourceStorageModeShared
                                        deallocator:nil];
    if (!b) { fprintf(stderr, "[bw2] FATAL: d3d9: cannot wrap guest memory 0x%08X as a Metal buffer\n", va); abort(); }
    g_arenas = realloc(g_arenas, sizeof *g_arenas * (g_narenas + 1));
    struct arena *a = &g_arenas[g_narenas];
    memset(a, 0, sizeof *a);
    a->va = va, a->size = size, a->buf = (__bridge_retained void *)b;
    a->free = grow(NULL, &a->capfree, sizeof *a->free);
    a->free[0] = (struct extent){ 0, size }, a->nfree = 1;
    return g_narenas++;
}

static int take(struct arena *a, uint32_t size, uint32_t *off) {
    for (uint32_t i = 0; i < a->nfree; i++)
        if (a->free[i].size >= size) {
            *off = a->free[i].off;
            a->free[i].off += size, a->free[i].size -= size;
            if (!a->free[i].size) {
                memmove(&a->free[i], &a->free[i + 1], sizeof *a->free * (a->nfree - i - 1));
                a->nfree--;
            }
            return 1;
        }
    return 0;
}

uint32_t mtl_heap_alloc(uint32_t size) {
    uint32_t n = (size + ALIGN - 1) & ~(ALIGN - 1), off = 0, ai = UINT32_MAX;
    if (!n) n = ALIGN;
    for (uint32_t i = 0; i < g_narenas && ai == UINT32_MAX; i++)
        if (take(&g_arenas[i], n, &off)) ai = i;
    if (ai == UINT32_MAX) {
        ai = new_arena(n > ARENA ? (n + 0xFFFFu) & ~0xFFFFu : ARENA);
        if (!take(&g_arenas[ai], n, &off)) abort();
    }
    uint32_t va = g_arenas[ai].va + off;
    memset(hle_ptr(va), 0, n);
    if (g_nblocks == g_capblocks) g_blocks = grow(g_blocks, &g_capblocks, sizeof *g_blocks);
    g_blocks[g_nblocks++] = (struct block){ va, n, ai };
    return va;
}

static int find_block(uint32_t va) {
    for (uint32_t i = g_nblocks; i-- > 0;)
        if (g_blocks[i].va == va) return (int)i;
    return -1;
}

void mtl_heap_free(uint32_t va) {
    int bi = find_block(va);
    if (bi < 0) { fprintf(stderr, "[bw2] FATAL: d3d9: free of unknown buffer block 0x%08X\n", va); abort(); }
    struct block b = g_blocks[bi];
    g_blocks[bi] = g_blocks[--g_nblocks];
    struct arena *a = &g_arenas[b.arena];
    uint32_t off = b.va - a->va, i = 0;
    while (i < a->nfree && a->free[i].off < off) i++;
    if (a->nfree == a->capfree) a->free = grow(a->free, &a->capfree, sizeof *a->free);
    memmove(&a->free[i + 1], &a->free[i], sizeof *a->free * (a->nfree - i));
    a->free[i] = (struct extent){ off, b.size };
    a->nfree++;
    if (i + 1 < a->nfree && a->free[i].off + a->free[i].size == a->free[i + 1].off) {
        a->free[i].size += a->free[i + 1].size;
        memmove(&a->free[i + 1], &a->free[i + 2], sizeof *a->free * (a->nfree - i - 2));
        a->nfree--;
    }
    if (i > 0 && a->free[i - 1].off + a->free[i - 1].size == a->free[i].off) {
        a->free[i - 1].size += a->free[i].size;
        memmove(&a->free[i], &a->free[i + 1], sizeof *a->free * (a->nfree - i - 1));
        a->nfree--;
    }
}

uint64_t mtl_serial(struct mtl_ctx *cc) { return ((__bridge MtlCtx *)(void *)cc).submitted + 1; }
int mtl_serial_done(struct mtl_ctx *cc, uint64_t serial) {
    return !cc || ((__bridge MtlCtx *)(void *)cc).completed >= serial;
}

void mtl_heap_free_after(struct mtl_ctx *c, uint64_t serial, uint32_t va) {
    if (mtl_serial_done(c, serial)) { mtl_heap_free(va); return; }
    if (g_npending == g_cappending) g_pending = grow(g_pending, &g_cappending, sizeof *g_pending);
    g_pending[g_npending++] = (struct pending){ va, c, serial };
}

void mtl_heap_collect(void) {
    for (uint32_t i = 0; i < g_npending;)
        if (mtl_serial_done(g_pending[i].ctx, g_pending[i].serial)) {
            mtl_heap_free(g_pending[i].va);
            g_pending[i] = g_pending[--g_npending];
        } else i++;
}
void mtl_heap_forget_ctx(struct mtl_ctx *c) {         /* its work is finished: free what waited on it */
    for (uint32_t i = 0; i < g_npending;)
        if (g_pending[i].ctx == c) { mtl_heap_free(g_pending[i].va); g_pending[i] = g_pending[--g_npending]; }
        else i++;
}

void *mtl_heap_buffer(uint32_t va, uint32_t *offset) {
    for (uint32_t i = 0; i < g_narenas; i++)
        if (va >= g_arenas[i].va && va - g_arenas[i].va < g_arenas[i].size) {
            *offset = va - g_arenas[i].va;
            return g_arenas[i].buf;
        }
    return NULL;
}
