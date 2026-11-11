/*
 * d3d9_draw.c - vertex declarations, FVFs and the draw calls (plan 4 Task 6),
 * and the state the fixed-function pipeline reads that is not a render state:
 * transforms, lights, the material, clip planes. Programmable shaders
 * (plan 5): creating, setting and getting them and their constants are here,
 * the objects in d3d9_shader.c. Pixel shaders are Task 4's: creating one exits 4.
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"
#include "ff.h"

/* ---- vertex declarations ---- */
struct d3d_vdecl { struct d3d_obj base; uint32_t device, n, fvf; uint8_t el[65][8]; };

static void destroy_vdecl(struct d3d_obj *o) {
    struct d3d_vdecl *v = (struct d3d_vdecl *)o;
    uint32_t dev = v->device;
    free(v);
    if (dev && d3d_alive(dev)) d3d_release(dev);
}
static struct d3d_vdecl *vdecl(uint32_t o) { return d3d_host(o, D3DK_VDECL); }

static uint32_t vdecl_new(uint32_t dev, const uint8_t (*el)[8], uint32_t n, uint32_t fvf) {
    struct d3d_vdecl *v = calloc(1, sizeof *v);
    v->base.destroy = destroy_vdecl, v->device = dev, v->n = n, v->fvf = fvf;
    memcpy(v->el, el, 8 * n);
    if (dev) d3d_addref(dev);
    return d3d_new(D3DK_VDECL, "IDirect3DVertexDeclaration9", &v->base);
}

static void el_set(uint8_t *e, uint32_t stream, uint32_t off, uint32_t type, uint32_t usage, uint32_t idx) {
    e[0] = (uint8_t)stream, e[1] = (uint8_t)(stream >> 8), e[2] = (uint8_t)off, e[3] = (uint8_t)(off >> 8);
    e[4] = (uint8_t)type, e[5] = 0, e[6] = (uint8_t)usage, e[7] = (uint8_t)idx;
}

/* An FVF as declaration elements (stream 0); 0 if the FVF is invalid. */
static uint32_t fvf_elements(uint32_t fvf, uint8_t (*el)[8]) {
    uint32_t n = 0, off = 0, pos = fvf & 0x400E;
    if (pos == 0x4002) el_set(el[n++], 0, off, 3, 0, 0), off += 16;            /* XYZW */
    else if (pos == 0x004) el_set(el[n++], 0, off, 3, 9, 0), off += 16;        /* XYZRHW: POSITIONT */
    else if (pos >= 0x002 && pos <= 0x00E) {
        el_set(el[n++], 0, off, 2, 0, 0), off += 12;
        uint32_t betas = (pos - 0x002) / 2;                                    /* XYZB1-5 */
        int last_idx = (fvf & 0x1000) || (fvf & 0x8000);
        if (betas) {
            uint32_t w = last_idx ? betas - 1 : betas;
            if (w) el_set(el[n++], 0, off, w - 1, 1, 0), off += 4 * w;
            if (last_idx) el_set(el[n++], 0, off, fvf & 0x8000 ? 4 : 5, 2, 0), off += 4;
        }
    } else return 0;
    if (fvf & 0x010) el_set(el[n++], 0, off, 2, 3, 0), off += 12;                /* NORMAL */
    if (fvf & 0x020) el_set(el[n++], 0, off, 0, 4, 0), off += 4;                 /* PSIZE */
    if (fvf & 0x040) el_set(el[n++], 0, off, 4, 10, 0), off += 4;                /* DIFFUSE */
    if (fvf & 0x080) el_set(el[n++], 0, off, 4, 10, 1), off += 4;                /* SPECULAR */
    uint32_t ntex = (fvf >> 8) & 0xF;
    if (ntex > 8) return 0;
    for (uint32_t t = 0; t < ntex; t++) {
        static const uint32_t comps[4] = { 2, 3, 4, 1 };
        uint32_t c = comps[(fvf >> (16 + 2 * t)) & 3];
        el_set(el[n++], 0, off, c - 1, 5, t), off += 4 * c;
    }
    return n;
}
__attribute__((unused)) static uint32_t fvf_stride(uint32_t fvf) {
    uint8_t el[32][8];
    uint32_t n = fvf_elements(fvf, el), end = 0;
    for (uint32_t i = 0; i < n; i++) {
        static const uint32_t sz[] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
        uint32_t e = (el[i][2] | el[i][3] << 8) + sz[el[i][4]];
        if (e > end) end = e;
    }
    return end;
}

static void dv_CreateVertexDeclaration(void) {   /* (this, D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) */
    (void)d3d_dev(A32(0));
    const uint8_t *e = APTR(1);
    if (!e || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    uint32_t n = 0;                                    /* up to 64 elements, then D3DDECL_END */
    while (n <= 64 && !(e[8 * n] == 0xFF && e[8 * n + 1] == 0)) {
        if (e[8 * n + 4] > 17 || e[8 * n + 5] != 0) RET(D3DERR_INVALIDCALL, 3);   /* types; only DEFAULT method */
        n++;
    }
    if (n > 64) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = vdecl_new(A32(0), (const uint8_t (*)[8])e, n, 0);
    RET(D3D_OK, 3);
}
static void vd_GetDevice(void) {
    struct d3d_vdecl *v = vdecl(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(v->device);
    MEM32(A32(1)) = v->device;
    RET(D3D_OK, 2);
}
static void vd_GetDeclaration(void) {      /* (this, D3DVERTEXELEMENT9*, UINT* count incl. the end) */
    struct d3d_vdecl *v = vdecl(A32(0));
    if (!A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = v->n + 1;
    if (A32(1)) {
        memcpy(APTR(1), v->el, 8 * v->n);
        static const uint8_t end[8] = { 0xFF, 0, 0, 0, 17, 0, 0, 0 };
        memcpy((uint8_t *)APTR(1) + 8 * v->n, end, 8);
    }
    RET(D3D_OK, 3);
}
static void dv_SetVertexDeclaration(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1)) (void)vdecl(A32(1));
    struct d3d_state *w = d3d_ws(d, SBW_VDECL, 0, 0);
    d3d_bind(&w->vdecl, A32(1));
    w->fvf = A32(1) ? vdecl(A32(1))->fvf : 0;
    w->dirty |= DIRTY_INPUT;
    RET(D3D_OK, 2);
}
static void dv_GetVertexDeclaration(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->st.vdecl;
    if (d->st.vdecl) d3d_addref(d->st.vdecl);
    RET(D3D_OK, 2);
}
static void dv_SetFVF(void) {              /* (this, fvf): the FVF's own declaration */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t fvf = A32(1);
    struct d3d_state *w = d3d_ws(d, SBW_VDECL, 0, 0);
    if (!fvf) { w->fvf = 0; RET(D3D_OK, 2); }
    uint8_t el[32][8];
    uint32_t n = fvf_elements(fvf, el);
    if (!n) RET(D3DERR_INVALIDCALL, 2);
    if (!w->vdecl || vdecl(w->vdecl)->fvf != fvf) {
        uint32_t v = vdecl_new(0, (const uint8_t (*)[8])el, n, fvf);
        d3d_bind(&w->vdecl, v);
        d3d_release(v);
    }
    w->fvf = fvf;
    w->dirty |= DIRTY_INPUT;
    RET(D3D_OK, 2);
}
static void dv_GetFVF(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->st.fvf;
    RET(D3D_OK, 2);
}

/* ---- draws ---- */
static uint32_t prim_vertices(uint32_t type, uint32_t count) {
    switch (type) {
    case 1: return count;
    case 2: return 2 * count;
    case 3: return count + 1;
    case 4: return 3 * count;
    case 5: case 6: return count + 2;
    default: return 0;
    }
}

/* A fan as a triangle list: 32-bit indices in GPU-visible memory, freed after this frame. */
static uint32_t fan_indices(struct d3d_device *d, uint32_t prims, const void *src, int src32, uint32_t first) {
    uint32_t va = mtl_heap_alloc(prims * 12);
    uint32_t *o = hle_ptr(va);
    for (uint32_t i = 0; i < prims; i++) {
        uint32_t k[3] = { 0, i + 1, i + 2 };
        for (int j = 0; j < 3; j++)
            o[3 * i + j] = !src ? first + k[j] : src32 ? ((const uint32_t *)src)[k[j]] : ((const uint16_t *)src)[k[j]];
    }
    mtl_heap_free_after(d->mtl, mtl_serial(d->mtl), va);
    return va;
}

/* A UP draw's own stream 0 and indices, already in GPU-visible memory. */
struct up { uint32_t vva, stride, iva; int i32; };

/* Record a draw with the current state. Indexed: ib, start index and base vertex; else start vertex. */
static uint32_t draw(struct d3d_device *d, uint32_t type, uint32_t prims, uint32_t start, int32_t base, int indexed,
                     const struct up *up) {
    if (!prims) return D3D_OK;
    uint32_t nv = prim_vertices(type, prims);
    if (!nv) return D3DERR_INVALIDCALL;
    if (d->st.ps) d3d_fail("draw with a pixel shader (plan 5 Task 4)");
    if (!d->st.vdecl) return D3DERR_INVALIDCALL;
    struct d3d_vdecl *v = vdecl(d->st.vdecl);
    struct ff_draw dr;
    struct ff_vkey vk;
    struct ff_pkey pk;
    struct ff_vs_u vu;
    struct ff_ps_u pu;
    struct ff_prog vsp;
    struct d3d_vs_consts vsc;
    memset(&dr, 0, sizeof dr);
    d3d_apply_targets(d);
    ff_build(d, (const uint8_t (*)[8])v->el, v->n, type, d->st.vs != 0, &dr, &vk, &pk, &vu, &pu);
    if (d->st.vs) {
        d3d_vs_build(d, d->st.vs, (const uint8_t (*)[8])v->el, v->n, type, &dr, &vu, &vsp, &vsc);
        dr.vs = &vsp;
    }
    for (uint32_t i = 0; i < dr.nattr; i++) {
        uint32_t s = dr.attr[i].stream;
        if (dr.stream_buf[s]) continue;
        if (up && s == 0) {
            dr.stream_buf[0] = mtl_heap_buffer(up->vva, &dr.stream_off[0]);
            dr.stream_stride[0] = up->stride;
            continue;
        }
        if (!d->st.stream[s]) return D3DERR_INVALIDCALL;
        uint32_t off;
        dr.stream_buf[s] = d3d_buffer_for_draw(d->st.stream[s], &off);
        dr.stream_off[s] = off + d->st.stream_off[s];
        dr.stream_stride[s] = d->st.stream_stride[s];
    }
    dr.prim = type;
    if (indexed && up) {
        dr.indexed = 1;
        if (type == 6) {
            uint32_t va = fan_indices(d, prims, hle_ptr(up->iva), up->i32, 0);
            dr.ibuf = mtl_heap_buffer(va, &dr.ioff), dr.index32 = 1, dr.count = 3 * prims, dr.prim = 4;
        } else dr.ibuf = mtl_heap_buffer(up->iva, &dr.ioff), dr.index32 = up->i32, dr.count = nv;
    } else if (indexed) {
        if (!d->st.indices) return D3DERR_INVALIDCALL;
        struct d3d_buffer *ib = d3d_buf(d->st.indices);
        int i32 = ib->fvf_fmt == D3DFMT_INDEX32;
        uint32_t isz = i32 ? 4 : 2;
        if ((uint64_t)(start + nv) * isz > ib->size) return D3DERR_INVALIDCALL;
        dr.indexed = 1, dr.base_vertex = base;
        if (type == 6) {
            uint32_t va = fan_indices(d, prims, (uint8_t *)hle_ptr(ib->mem) + start * isz, i32, 0);
            dr.ibuf = mtl_heap_buffer(va, &dr.ioff), dr.index32 = 1, dr.count = 3 * prims, dr.prim = 4;
        } else {
            uint32_t off;
            dr.ibuf = d3d_buffer_for_draw(d->st.indices, &off);
            dr.ioff = off + start * isz, dr.index32 = i32, dr.count = nv;
            if (dr.ioff & 3) {                         /* Metal wants the index offset 4-aligned: copy them */
                uint32_t va = mtl_heap_alloc(nv * isz);
                memcpy(hle_ptr(va), (uint8_t *)hle_ptr(ib->mem) + start * isz, nv * isz);
                mtl_heap_free_after(d->mtl, mtl_serial(d->mtl), va);
                dr.ibuf = mtl_heap_buffer(va, &dr.ioff);
            }
        }
    } else if (type == 6) {
        uint32_t va = fan_indices(d, prims, NULL, 0, start);
        dr.indexed = 1, dr.ibuf = mtl_heap_buffer(va, &dr.ioff), dr.index32 = 1, dr.count = 3 * prims, dr.prim = 4;
    } else dr.start = start, dr.count = nv;
    mtl_draw(d->mtl, &dr);
    return D3D_OK;
}

static void dv_DrawPrimitive(void) {       /* (this, type, start vertex, primitives) */
    struct d3d_device *d = d3d_dev(A32(0));
    RET(draw(d, A32(1), A32(3), A32(2), 0, 0, NULL), 4);
}
static void dv_DrawIndexedPrimitive(void) {   /* (this, type, base vertex, min index, vertices, start index, primitives) */
    struct d3d_device *d = d3d_dev(A32(0));
    RET(draw(d, A32(1), A32(6), A32(5), (int32_t)A32(2), 1, NULL), 7);
}

/* The UP draws: the guest's data copied into GPU-visible memory (freed after this frame) as stream 0. */
static uint32_t up_copy(struct d3d_device *d, uint32_t src, uint32_t bytes) {
    uint32_t va = mtl_heap_alloc(bytes);
    memcpy(hle_ptr(va), hle_ptr(src), bytes);
    mtl_heap_free_after(d->mtl, mtl_serial(d->mtl), va);
    return va;
}
static uint32_t draw_up(struct d3d_device *d, uint32_t type, uint32_t prims, uint32_t vdata, uint32_t vbytes,
                        uint32_t stride, uint32_t idata, uint32_t ibytes, int i32) {
    struct up u = { up_copy(d, vdata, vbytes), stride, idata ? up_copy(d, idata, ibytes) : 0, i32 };
    d3d_bind(&d->st.stream[0], 0);                     /* as documented: the UP draws leave these unset */
    d->st.stream_off[0] = d->st.stream_stride[0] = 0;
    if (idata) d3d_bind(&d->st.indices, 0);
    return draw(d, type, prims, 0, 0, idata != 0, &u);
}
static void dv_DrawPrimitiveUP(void) {     /* (this, type, primitives, data, stride) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t nv = prim_vertices(A32(1), A32(2));
    if (!A32(3) || !nv) RET(A32(2) ? D3DERR_INVALIDCALL : D3D_OK, 5);
    RET(draw_up(d, A32(1), A32(2), A32(3), nv * A32(4), A32(4), 0, 0, 0), 5);
}
static void dv_DrawIndexedPrimitiveUP(void) {   /* (this, type, min index, vertices, primitives, indices, format, data, stride) */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t ni = prim_vertices(A32(1), A32(4)), i32 = A32(6) == D3DFMT_INDEX32;
    if (!A32(5) || !A32(7) || !ni) RET(A32(4) ? D3DERR_INVALIDCALL : D3D_OK, 9);
    RET(draw_up(d, A32(1), A32(4), A32(7), (A32(2) + A32(3)) * A32(8), A32(8), A32(5), ni * (i32 ? 4 : 2), i32), 9);
}

static void dv_ProcessVertices(void) { (void)d3d_dev(A32(0)); d3d_fail("ProcessVertices is not supported"); }

/* ---- transforms, lights, material, clip planes ---- */
static int xform_index(uint32_t t) { return t < 512 ? (int)t : -1; }
static void dv_SetTransform(void) {        /* (this, D3DTRANSFORMSTATETYPE, D3DMATRIX*) */
    struct d3d_device *d = d3d_dev(A32(0));
    int i = xform_index(A32(1));
    if (i < 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    struct d3d_state *w = d3d_ws(d, SBW_XFORM, (uint32_t)i, 0);
    memcpy(w->xform[i], APTR(2), 64);
    w->dirty |= DIRTY_XFORM;
    RET(D3D_OK, 3);
}
static void dv_GetTransform(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    int i = xform_index(A32(1));
    if (i < 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), d->st.xform[i], 64);
    RET(D3D_OK, 3);
}
static void dv_MultiplyTransform(void) {   /* M = M * given */
    struct d3d_device *d = d3d_dev(A32(0));
    int i = xform_index(A32(1));
    if (i < 0 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    float a[16], b[16], o[16];
    memcpy(a, d->st.xform[i], 64), memcpy(b, APTR(2), 64);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    struct d3d_state *w = d3d_ws(d, SBW_XFORM, (uint32_t)i, 0);
    memcpy(w->xform[i], o, 64);
    w->dirty |= DIRTY_XFORM;
    RET(D3D_OK, 3);
}
static struct d3d_light *light(struct d3d_state *st, uint32_t i, int grow) { return d3d_light_at(st, i, grow); }
static void dv_SetLight(void) {            /* (this, index, D3DLIGHT9*) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(2)) RET(D3DERR_INVALIDCALL, 3);
    uint32_t type = MEM32(A32(2));
    if (type < 1 || type > 3) RET(D3DERR_INVALIDCALL, 3);
    struct d3d_state *w = d3d_ws(d, SBW_LIGHT, A32(1), 0);
    struct d3d_light *l = light(w, A32(1), 1);
    memcpy(l->data, APTR(2), 104);
    l->set = 1;
    w->dirty |= DIRTY_LIGHT;
    RET(D3D_OK, 3);
}
static void dv_GetLight(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    struct d3d_light *l = light(&d->st, A32(1), 0);
    if (!A32(2) || !l || !l->set) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), l->data, 104);
    RET(D3D_OK, 3);
}
static void dv_LightEnable(void) {         /* (this, index, enable): an unset light becomes the default one */
    struct d3d_device *d = d3d_dev(A32(0));
    struct d3d_state *w = d3d_ws(d, SBW_LIGHT, A32(1), 0);
    struct d3d_light *l = light(w, A32(1), 1);
    if (!l->set) {
        memset(l->data, 0, sizeof l->data);
        l->data[0] = 3;                                 /* directional, white diffuse, direction (0, 0, 1) */
        float one = 1.0f;
        memcpy(&l->data[1], &one, 4), memcpy(&l->data[2], &one, 4), memcpy(&l->data[3], &one, 4);
        memcpy(&l->data[18], &one, 4);
        l->set = 1;
    }
    l->enabled = A32(2) != 0;
    w->dirty |= DIRTY_LIGHT;
    RET(D3D_OK, 3);
}
static void dv_GetLightEnable(void) {      /* (this, index, BOOL*): TRUE is 128 for an enabled light, as D3D */
    struct d3d_device *d = d3d_dev(A32(0));
    struct d3d_light *l = light(&d->st, A32(1), 0);
    if (!A32(2) || !l || !l->set) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = l->enabled ? 128 : 0;
    RET(D3D_OK, 3);
}
static void dv_SetMaterial(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    struct d3d_state *w = d3d_ws(d, SBW_MATERIAL, 0, 0);
    memcpy(w->material, APTR(1), 68);
    w->dirty |= DIRTY_LIGHT;
    RET(D3D_OK, 2);
}
static void dv_GetMaterial(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    memcpy(APTR(1), d->st.material, 68);
    RET(D3D_OK, 2);
}
static void dv_SetClipPlane(void) {        /* (this, index, float[4]) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) >= 6 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(d3d_ws(d, SBW_CLIP, A32(1), 0)->clip[A32(1)], APTR(2), 16);
    RET(D3D_OK, 3);
}
static void dv_GetClipPlane(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1) >= 6 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(APTR(2), d->st.clip[A32(1)], 16);
    RET(D3D_OK, 3);
}

/* ---- programmable shaders (plan 5) ---- */
static void dv_CreateVertexShader(void) {  /* (this, const DWORD* function, IDirect3DVertexShader9**) */
    (void)d3d_dev(A32(0));
    uint32_t out = 0, hr = d3d_vs_create(A32(0), A32(1) ? APTR(1) : NULL, A32(2) ? &out : NULL);
    if (!hr) MEM32(A32(2)) = out;
    RET(hr, 3);
}
static void dv_CreatePixelShader(void) { d3d_later("IDirect3DDevice9::CreatePixelShader"); }
static void dv_SetVertexShader(void) {     /* (this, shader or NULL: the fixed vertex pipeline) */
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1)) (void)d3d_host(A32(1), D3DK_VSHADER);
    d3d_bind(&d3d_ws(d, SBW_VS, 0, 0)->vs, A32(1));
    RET(D3D_OK, 2);
}
static void dv_SetPixelShader(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (A32(1)) d3d_fail("SetPixelShader of a shader this plan cannot have made");
    d3d_bind(&d3d_ws(d, SBW_PS, 0, 0)->ps, 0);
    RET(D3D_OK, 2);
}
static void dv_GetVertexShader(void) {     /* AddRef'd, as every D3D9 Get of an object */
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->st.vs;
    if (d->st.vs) d3d_addref(d->st.vs);
    RET(D3D_OK, 2);
}
static void dv_GetPixelShader(void) {
    struct d3d_device *d = d3d_dev(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d->st.ps;
    if (d->st.ps) d3d_addref(d->st.ps);
    RET(D3D_OK, 2);
}
/* Constants: (this, start register, data, count); F in vec4s, I in int4s, B in BOOLs. */
static uint32_t consts(void *base, uint32_t nregs, uint32_t elem, uint32_t start, uint32_t data, uint32_t n, int set) {
    if (!data || start > nregs || n > nregs - start) return D3DERR_INVALIDCALL;
    uint8_t *p = (uint8_t *)base + (size_t)start * elem;
    if (set) memcpy(p, hle_ptr(data), (size_t)n * elem);
    else memcpy(hle_ptr(data), p, (size_t)n * elem);
    return D3D_OK;
}
#define CONSTS(NAME, FIELD, NREGS, ELEM, SET, W) \
    static void NAME(void) { struct d3d_device *d = d3d_dev(A32(0)); \
        struct d3d_state *w = SET && A32(1) <= NREGS && A32(3) <= NREGS - A32(1) ? d3d_ws(d, W, A32(1), A32(3)) : &d->st; \
        RET(consts(w->FIELD, NREGS, ELEM, A32(1), A32(2), A32(3), SET), 4); }
CONSTS(dv_SetVSF, vsf, 256, 16, 1, SBW_VSF)
CONSTS(dv_GetVSF, vsf, 256, 16, 0, SBW_VSF)
CONSTS(dv_SetVSI, vsi, 16, 16, 1, SBW_VSI)
CONSTS(dv_GetVSI, vsi, 16, 16, 0, SBW_VSI)
CONSTS(dv_SetVSB, vsb, 16, 4, 1, SBW_VSB)
CONSTS(dv_GetVSB, vsb, 16, 4, 0, SBW_VSB)
CONSTS(dv_SetPSF, psf, 224, 16, 1, SBW_PSF)
CONSTS(dv_GetPSF, psf, 224, 16, 0, SBW_PSF)
CONSTS(dv_SetPSI, psi, 16, 16, 1, SBW_PSI)
CONSTS(dv_GetPSI, psi, 16, 16, 0, SBW_PSI)
CONSTS(dv_SetPSB, psb, 16, 4, 1, SBW_PSB)
CONSTS(dv_GetPSB, psb, 16, 4, 0, SBW_PSB)

static void dv_ValidateDevice(void) {      /* (this, DWORD* passes) */
    (void)d3d_dev(A32(0));
    if (A32(1)) MEM32(A32(1)) = 1;
    RET(D3D_OK, 2);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::CreateVertexDeclaration", dv_CreateVertexDeclaration },
    { "IDirect3DDevice9::SetVertexDeclaration", dv_SetVertexDeclaration },
    { "IDirect3DDevice9::GetVertexDeclaration", dv_GetVertexDeclaration },
    { "IDirect3DDevice9::SetFVF", dv_SetFVF },
    { "IDirect3DDevice9::GetFVF", dv_GetFVF },
    { "IDirect3DVertexDeclaration9::GetDevice", vd_GetDevice },
    { "IDirect3DVertexDeclaration9::GetDeclaration", vd_GetDeclaration },
    { "IDirect3DDevice9::DrawPrimitive", dv_DrawPrimitive },
    { "IDirect3DDevice9::DrawIndexedPrimitive", dv_DrawIndexedPrimitive },
    { "IDirect3DDevice9::DrawPrimitiveUP", dv_DrawPrimitiveUP },
    { "IDirect3DDevice9::DrawIndexedPrimitiveUP", dv_DrawIndexedPrimitiveUP },
    { "IDirect3DDevice9::ProcessVertices", dv_ProcessVertices },
    { "IDirect3DDevice9::SetTransform", dv_SetTransform },
    { "IDirect3DDevice9::GetTransform", dv_GetTransform },
    { "IDirect3DDevice9::MultiplyTransform", dv_MultiplyTransform },
    { "IDirect3DDevice9::SetLight", dv_SetLight },
    { "IDirect3DDevice9::GetLight", dv_GetLight },
    { "IDirect3DDevice9::LightEnable", dv_LightEnable },
    { "IDirect3DDevice9::GetLightEnable", dv_GetLightEnable },
    { "IDirect3DDevice9::SetMaterial", dv_SetMaterial },
    { "IDirect3DDevice9::GetMaterial", dv_GetMaterial },
    { "IDirect3DDevice9::SetClipPlane", dv_SetClipPlane },
    { "IDirect3DDevice9::GetClipPlane", dv_GetClipPlane },
    { "IDirect3DDevice9::CreateVertexShader", dv_CreateVertexShader },
    { "IDirect3DDevice9::CreatePixelShader", dv_CreatePixelShader },
    { "IDirect3DDevice9::SetVertexShader", dv_SetVertexShader },
    { "IDirect3DDevice9::SetPixelShader", dv_SetPixelShader },
    { "IDirect3DDevice9::GetVertexShader", dv_GetVertexShader },
    { "IDirect3DDevice9::GetPixelShader", dv_GetPixelShader },
    { "IDirect3DDevice9::SetVertexShaderConstantF", dv_SetVSF },
    { "IDirect3DDevice9::GetVertexShaderConstantF", dv_GetVSF },
    { "IDirect3DDevice9::SetVertexShaderConstantI", dv_SetVSI },
    { "IDirect3DDevice9::GetVertexShaderConstantI", dv_GetVSI },
    { "IDirect3DDevice9::SetVertexShaderConstantB", dv_SetVSB },
    { "IDirect3DDevice9::GetVertexShaderConstantB", dv_GetVSB },
    { "IDirect3DDevice9::SetPixelShaderConstantF", dv_SetPSF },
    { "IDirect3DDevice9::GetPixelShaderConstantF", dv_GetPSF },
    { "IDirect3DDevice9::SetPixelShaderConstantI", dv_SetPSI },
    { "IDirect3DDevice9::GetPixelShaderConstantI", dv_GetPSI },
    { "IDirect3DDevice9::SetPixelShaderConstantB", dv_SetPSB },
    { "IDirect3DDevice9::GetPixelShaderConstantB", dv_GetPSB },
    { "IDirect3DDevice9::ValidateDevice", dv_ValidateDevice },
    { 0, 0 } };
void d3d9_draw_register(void) { d3d_impls(g_impls); }
