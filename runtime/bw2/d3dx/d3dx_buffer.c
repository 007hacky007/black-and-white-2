/*
 * d3dx_buffer.c - ID3DXBuffer and D3DXCreateBuffer (plan 4 Task 9).
 */
#include <stdlib.h>
#include "d3d_com.h"
#include "d3dx.h"

struct d3dx_buffer { struct d3d_obj base; uint32_t mem, size; };

static void destroy(struct d3d_obj *o) {
    struct d3dx_buffer *b = (struct d3dx_buffer *)o;
    if (b->mem) hle_free(hle_ptr(b->mem));
    free(b);
}
uint32_t d3dx_buffer_new(uint32_t size) {
    struct d3dx_buffer *b = calloc(1, sizeof *b);
    b->base.destroy = destroy, b->size = size, b->mem = hle_va(hle_alloc(size ? size : 4));
    return d3d_new(D3DK_D3DXBUFFER, "ID3DXBuffer", &b->base);
}
uint32_t d3dx_buffer_ptr(uint32_t o) { return ((struct d3dx_buffer *)d3d_host(o, D3DK_D3DXBUFFER))->mem; }

static void b_GetBufferPointer(void) { RET(d3dx_buffer_ptr(A32(0)), 1); }
static void b_GetBufferSize(void) { RET(((struct d3dx_buffer *)d3d_host(A32(0), D3DK_D3DXBUFFER))->size, 1); }
static void x_D3DXCreateBuffer(void) {         /* (bytes, ID3DXBuffer**) */
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d3dx_buffer_new(A32(0));
    RET(D3D_OK, 2);
}

static const win32hle_shim g_impls[] = {
    { "ID3DXBuffer::GetBufferPointer", b_GetBufferPointer }, { "ID3DXBuffer::GetBufferSize", b_GetBufferSize },
    { 0, 0 } };
static const win32hle_shim g_exports[] = { { "D3DXCreateBuffer", x_D3DXCreateBuffer }, { 0, 0 } };
void d3dx_buffer_register(void) { d3d_impls(g_impls); win32hle_register(g_exports); }
