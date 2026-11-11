/*
 * d3d9_buffer.c - IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9, and the
 * stream and index bindings (plan 4 Task 5). A buffer's bytes live in
 * GPU-visible guest memory (mtl_buffer.m): a lock returns them, the GPU reads
 * them, and nothing is copied. A lock that could change bytes a recorded draw
 * has yet to read moves the buffer to fresh memory first: DISCARD leaves the
 * new memory as it is, a plain lock copies the old bytes over.
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

#define D3DRTYPE_VERTEXBUFFER_ 6u
#define D3DRTYPE_INDEXBUFFER_ 7u

struct d3d_buffer *d3d_buf(uint32_t obj) {
    struct d3d_obj *o = d3d_host_any(obj);
    if (!o || (o->kind != D3DK_VB && o->kind != D3DK_IB)) d3d_fail("0x%08X is not a live buffer", obj);
    return (struct d3d_buffer *)o;
}

static int in_use(struct d3d_buffer *b) { return b->used_serial && !mtl_serial_done(b->used_ctx, b->used_serial); }

static void retire(struct d3d_buffer *b) {          /* the old memory goes once its last reader is done */
    if (in_use(b)) mtl_heap_free_after(b->used_ctx, b->used_serial, b->mem);
    else mtl_heap_free(b->mem);
}

static void destroy_buffer(struct d3d_obj *o) {
    struct d3d_buffer *b = (struct d3d_buffer *)o;
    retire(b);
    if (d3d_alive(b->device)) {
        struct d3d_device *d = d3d_host(b->device, D3DK_DEVICE);
        if (b->counts_for_reset) d->default_pool_live--;
    }
    uint32_t dev = b->device;
    free(b);
    d3d_release(dev);
}

static uint32_t create(uint32_t dev, enum d3d_kind kind, uint32_t size, uint32_t usage, uint32_t fvf_fmt,
                       uint32_t pool, uint32_t out) {
    struct d3d_device *d = d3d_dev(dev);
    if (!out || !size || pool > D3DPOOL_SCRATCH || pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if ((usage & D3DUSAGE_DYNAMIC) && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    if (kind == D3DK_IB && fvf_fmt != D3DFMT_INDEX16 && fvf_fmt != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    struct d3d_buffer *b = calloc(1, sizeof *b);
    b->base.destroy = destroy_buffer;
    b->device = dev, b->size = size, b->usage = usage, b->fvf_fmt = fvf_fmt, b->pool = pool;
    b->mem = mtl_heap_alloc(size);
    b->counts_for_reset = pool == D3DPOOL_DEFAULT;
    if (b->counts_for_reset) d->default_pool_live++;
    d3d_addref(dev);
    MEM32(out) = d3d_new(kind, kind == D3DK_VB ? "IDirect3DVertexBuffer9" : "IDirect3DIndexBuffer9", &b->base);
    return D3D_OK;
}
static void dv_CreateVertexBuffer(void) {  /* (this, length, usage, fvf, pool, IDirect3DVertexBuffer9**, shared) */
    RET(A32(6) ? D3DERR_INVALIDCALL : create(A32(0), D3DK_VB, A32(1), A32(2), A32(3), A32(4), A32(5)), 7);
}
static void dv_CreateIndexBuffer(void) {   /* (this, length, usage, format, pool, IDirect3DIndexBuffer9**, shared) */
    RET(A32(6) ? D3DERR_INVALIDCALL : create(A32(0), D3DK_IB, A32(1), A32(2), A32(3), A32(4), A32(5)), 7);
}

static void bf_Lock(void) {                /* (this, offset, size, void**, flags) */
    struct d3d_buffer *b = d3d_buf(A32(0));
    uint32_t off = A32(1), size = A32(2), out = A32(3), flags = A32(4);
    if (!out) RET(D3DERR_INVALIDCALL, 5);
    if (!size && !off) size = b->size;
    if (off > b->size || size > b->size - off) RET(D3DERR_INVALIDCALL, 5);
    if (in_use(b) && !(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
        uint32_t fresh = mtl_heap_alloc(b->size);
        if (!(flags & D3DLOCK_DISCARD)) memcpy(hle_ptr(fresh), hle_ptr(b->mem), b->size);
        retire(b);
        b->mem = fresh, b->used_serial = 0, b->used_ctx = NULL;
    }
    b->locks++;
    MEM32(out) = b->mem + off;
    RET(D3D_OK, 5);
}
static void bf_Unlock(void) {
    struct d3d_buffer *b = d3d_buf(A32(0));
    if (!b->locks) RET(D3DERR_INVALIDCALL, 1);
    b->locks--;
    RET(D3D_OK, 1);
}
static void vb_GetDesc(void) {             /* (this, D3DVERTEXBUFFER_DESC*) */
    struct d3d_buffer *b = d3d_buf(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(D3DERR_INVALIDCALL, 2);
    MEM32(p) = D3DFMT_VERTEXDATA, MEM32(p + 4) = D3DRTYPE_VERTEXBUFFER_, MEM32(p + 8) = b->usage;
    MEM32(p + 12) = b->pool, MEM32(p + 16) = b->size, MEM32(p + 20) = b->fvf_fmt;
    RET(D3D_OK, 2);
}
static void ib_GetDesc(void) {             /* (this, D3DINDEXBUFFER_DESC*) */
    struct d3d_buffer *b = d3d_buf(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(D3DERR_INVALIDCALL, 2);
    MEM32(p) = b->fvf_fmt, MEM32(p + 4) = D3DRTYPE_INDEXBUFFER_, MEM32(p + 8) = b->usage;
    MEM32(p + 12) = b->pool, MEM32(p + 16) = b->size;
    RET(D3D_OK, 2);
}
static void bf_GetDevice(void) {
    struct d3d_buffer *b = d3d_buf(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(b->device);
    MEM32(A32(1)) = b->device;
    RET(D3D_OK, 2);
}
static void bf_SetPriority(void) { (void)d3d_buf(A32(0)); RET(0, 2); }
static void bf_GetPriority(void) { (void)d3d_buf(A32(0)); RET(0, 1); }
static void bf_PreLoad(void) { (void)d3d_buf(A32(0)); RET(0, 1); }
static void bf_GetType(void) {
    RET(d3d_buf(A32(0))->base.kind == D3DK_VB ? D3DRTYPE_VERTEXBUFFER_ : D3DRTYPE_INDEXBUFFER_, 1);
}

void *d3d_buffer_for_draw(uint32_t obj, uint32_t *offset) {
    struct d3d_buffer *b = d3d_buf(obj);
    struct d3d_device *d = d3d_host(b->device, D3DK_DEVICE);
    b->used_ctx = d->mtl, b->used_serial = mtl_serial(d->mtl);
    return mtl_heap_buffer(b->mem, offset);
}

/* ---- bindings ---- */
static void dv_SetStreamSource(void) {     /* (this, stream, vb, offset, stride) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t i = A32(1);
    if (i >= 16) RET(D3DERR_INVALIDCALL, 5);
    if (A32(2) && d3d_buf(A32(2))->base.kind != D3DK_VB) RET(D3DERR_INVALIDCALL, 5);
    d3d_bind(&d->st.stream[i], A32(2));
    d->st.stream_off[i] = A32(3), d->st.stream_stride[i] = A32(4);
    d->st.dirty |= DIRTY_INPUT;
    RET(D3D_OK, 5);
}
static void dv_GetStreamSource(void) {     /* (this, stream, vb*, offset*, stride*) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t i = A32(1);
    if (i >= 16 || !A32(2) || !A32(3) || !A32(4)) RET(D3DERR_INVALIDCALL, 5);
    MEM32(A32(2)) = d->st.stream[i];
    if (d->st.stream[i]) d3d_addref(d->st.stream[i]);
    MEM32(A32(3)) = d->st.stream_off[i], MEM32(A32(4)) = d->st.stream_stride[i];
    RET(D3D_OK, 5);
}
static void dv_SetStreamSourceFreq(void) { /* (this, stream, divider) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) >= 16) RET(D3DERR_INVALIDCALL, 3);
    if (A32(2) != 1) d3d_fail("SetStreamSourceFreq(%u, 0x%X): instancing is not supported", A32(1), A32(2));
    d->st.stream_freq[A32(1)] = 1;
    RET(D3D_OK, 3);
}
static void dv_GetStreamSourceFreq(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) >= 16 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d->st.stream_freq[A32(1)];
    RET(D3D_OK, 3);
}
static void dv_SetIndices(void) {          /* (this, ib) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) && d3d_buf(A32(1))->base.kind != D3DK_IB) RET(D3DERR_INVALIDCALL, 2);
    d3d_bind(&d->st.indices, A32(1));
    RET(D3D_OK, 2);
}
static void dv_GetIndices(void) {          /* (this, ib*) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->st.indices;
    if (d->st.indices) d3d_addref(d->st.indices);
    RET(D3D_OK, 2);
}

#define BUFCOMMON(I) \
    { I "::GetDevice", bf_GetDevice }, { I "::SetPriority", bf_SetPriority }, { I "::GetPriority", bf_GetPriority }, \
    { I "::PreLoad", bf_PreLoad }, { I "::GetType", bf_GetType }, { I "::Lock", bf_Lock }, { I "::Unlock", bf_Unlock }
static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::CreateVertexBuffer", dv_CreateVertexBuffer },
    { "IDirect3DDevice9::CreateIndexBuffer", dv_CreateIndexBuffer },
    { "IDirect3DDevice9::SetStreamSource", dv_SetStreamSource },
    { "IDirect3DDevice9::GetStreamSource", dv_GetStreamSource },
    { "IDirect3DDevice9::SetStreamSourceFreq", dv_SetStreamSourceFreq },
    { "IDirect3DDevice9::GetStreamSourceFreq", dv_GetStreamSourceFreq },
    { "IDirect3DDevice9::SetIndices", dv_SetIndices },
    { "IDirect3DDevice9::GetIndices", dv_GetIndices },
    BUFCOMMON("IDirect3DVertexBuffer9"), BUFCOMMON("IDirect3DIndexBuffer9"),
    { "IDirect3DVertexBuffer9::GetDesc", vb_GetDesc },
    { "IDirect3DIndexBuffer9::GetDesc", ib_GetDesc },
    { 0, 0 } };
void d3d9_buffer_register(void) { d3d_impls(g_impls); }
