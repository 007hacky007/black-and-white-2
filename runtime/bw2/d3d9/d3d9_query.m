/*
 * d3d9_query.m - IDirect3DQuery9 (plan 4 Task 7): EVENT, done when the work
 * recorded before its END has finished on the GPU; OCCLUSION, the samples
 * that passed between BEGIN and END, from Metal's visibility results. Other
 * types are not available.
 */
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"

#define D3DQUERYTYPE_EVENT 8u
#define D3DQUERYTYPE_OCCLUSION 9u
#define D3DISSUE_END 1u
#define D3DISSUE_BEGIN 2u
#define D3DGETDATA_FLUSH 1u

struct d3d_query {
    struct d3d_obj base;
    uint32_t device, type;
    int issued, begun;
    uint64_t serial;
    struct mtl_occlusion occ;
};
static struct d3d_query *q_of(uint32_t o) { return d3d_host(o, D3DK_QUERY); }

static void destroy_query(struct d3d_obj *o) {
    struct d3d_query *q = (struct d3d_query *)o;
    if (q->begun && d3d_alive(q->device)) mtl_occlusion_end(((struct d3d_device *)d3d_host(q->device, D3DK_DEVICE))->mtl, &q->occ);
    uint32_t dev = q->device;
    free(q);
    if (d3d_alive(dev)) d3d_release(dev);
}

static void dv_CreateQuery(void) {         /* (this, type, IDirect3DQuery9**): NULL out asks for support */
    struct d3d_device *d = d3d_dev(A32(0));
    uint32_t type = A32(1);
    if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) RET(D3DERR_NOTAVAILABLE, 3);
    if (!A32(2)) RET(D3D_OK, 3);
    struct d3d_query *q = calloc(1, sizeof *q);
    q->base.destroy = destroy_query, q->device = d->base.guest, q->type = type;
    d3d_addref(d->base.guest);
    MEM32(A32(2)) = d3d_new(D3DK_QUERY, "IDirect3DQuery9", &q->base);
    RET(D3D_OK, 3);
}
static void q_GetDevice(void) {
    struct d3d_query *q = q_of(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    d3d_addref(q->device);
    MEM32(A32(1)) = q->device;
    RET(D3D_OK, 2);
}
static void q_GetType(void) { RET(q_of(A32(0))->type, 1); }
static void q_GetDataSize(void) { (void)q_of(A32(0)); RET(4, 1); }
static void q_Issue(void) {                /* (this, flags) */
    struct d3d_query *q = q_of(A32(0));
    struct d3d_device *d = d3d_dev(q->device);
    uint32_t f = A32(1);
    if (q->type == D3DQUERYTYPE_EVENT) {
        if (f & D3DISSUE_END) q->serial = mtl_serial(d->mtl), q->issued = 1;
        RET(D3D_OK, 2);
    }
    if (f & D3DISSUE_BEGIN) {
        if (q->begun) mtl_occlusion_end(d->mtl, &q->occ);
        d3d_apply_targets(d);
        mtl_occlusion_begin(d->mtl, &q->occ);
        q->begun = 1, q->issued = 0;
    }
    if ((f & D3DISSUE_END) && q->begun) {
        mtl_occlusion_end(d->mtl, &q->occ);
        q->begun = 0, q->issued = 1;
    }
    RET(D3D_OK, 2);
}
static void q_GetData(void) {              /* (this, data, size, flags): S_OK with data, or S_FALSE */
    struct d3d_query *q = q_of(A32(0));
    struct d3d_device *d = d3d_dev(q->device);
    uint32_t data = A32(1), size = A32(2), flags = A32(3);
    if (!q->issued) RET(q->begun ? S_FALSE_ : D3DERR_INVALIDCALL, 4);
    uint64_t serial = q->type == D3DQUERYTYPE_EVENT ? q->serial : q->occ.serial;
    if (!mtl_serial_done(d->mtl, serial) && (flags & D3DGETDATA_FLUSH) && serial == mtl_serial(d->mtl))
        mtl_commit_nowait(d->mtl);
    if (q->type == D3DQUERYTYPE_EVENT) {
        if (!mtl_serial_done(d->mtl, serial)) RET(S_FALSE_, 4);
        if (data && size >= 4) MEM32(data) = 1;
        RET(D3D_OK, 4);
    }
    uint64_t n;
    if (!mtl_occlusion_result(d->mtl, &q->occ, &n)) RET(S_FALSE_, 4);
    if (data && size >= 4) MEM32(data) = (uint32_t)n;
    RET(D3D_OK, 4);
}

static const win32hle_shim g_impls[] = {
    { "IDirect3DDevice9::CreateQuery", dv_CreateQuery },
    { "IDirect3DQuery9::GetDevice", q_GetDevice },
    { "IDirect3DQuery9::GetType", q_GetType },
    { "IDirect3DQuery9::GetDataSize", q_GetDataSize },
    { "IDirect3DQuery9::Issue", q_Issue },
    { "IDirect3DQuery9::GetData", q_GetData },
    { 0, 0 } };
void d3d9_query_register(void) { d3d_impls(g_impls); }
