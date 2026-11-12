/*
 * ff_key.c - from the device's state to the fixed-function pipeline's key,
 * uniforms and fixed state (plan 4 Task 6). Unsupported states stop loudly.
 */
#include <math.h>
#include <string.h>
#include "d3d9_device.h"
#include "ff.h"

/* ---- 4x4 matrices, D3D's row-vector convention: v' = v * M, M[row][col] ---- */
static void mmul(float *o, const float *a, const float *b) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}
static void mtranspose(float *o, const float *a) {
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t[j * 4 + i] = a[i * 4 + j];
    memcpy(o, t, sizeof t);
}
static int minverse(float *o, const float *m) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) { memcpy(o, m, sizeof inv); return 0; }
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return 1;
}
static void vxm(float *o, const float *v, const float *m) {      /* o = v * m, v a float4 */
    float t[4];
    for (int j = 0; j < 4; j++) t[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, t, sizeof t);
}

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void color4(float *o, uint32_t c) {
    o[0] = ((c >> 16) & 0xFF) / 255.0f, o[1] = ((c >> 8) & 0xFF) / 255.0f, o[2] = (c & 0xFF) / 255.0f, o[3] = (c >> 24) / 255.0f;
}

/* D3DDECLUSAGE */
enum { U_POSITION, U_BLENDWEIGHT, U_BLENDINDICES, U_NORMAL, U_PSIZE, U_TEXCOORD, U_TANGENT, U_BINORMAL,
       U_TESSFACTOR, U_POSITIONT, U_COLOR, U_FOG, U_DEPTH, U_SAMPLE };

static int type_comps(uint32_t t) {
    static const int n[] = { 1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4 };
    return t < 17 ? n[t] : 0;
}

/* Sampler i's texture (made current) and state into the draw; its type (0 none, 1 2D, 2 cube, 3 volume). */
int ff_bind_sampler(struct d3d_device *dv, uint32_t i, struct ff_draw *dr, struct ff_ps_u *pu) {
    struct d3d_state *st = &dv->st;
    uint32_t t = st->tex[i];
    dr->tex[i] = NULL;
    if (!t) return 0;
    struct d3d_texture *tx = d3d_tex(t);
    if (tx->autogen && tx->mip_dirty) {                  /* drawn into since: make the chain again first */
        mtl_tex_mipgen(dv->mtl, tx->tex);
        tx->mip_dirty = 0;
    }
    dr->tex[i] = tx->view;
    if (st->ss[i][11]) dr->tex[i] = d3d_tex_srgb_view(tx);
    const uint32_t *ss = st->ss[i];
    dr->smp[i] = (struct ff_sampler){ ss[1], ss[2], ss[3], ss[5], ss[6], ss[7], ss[10],
                                      ss[9] > tx->lod ? ss[9] : tx->lod, ss[4] };
    pu->lodbias[i][0] = f32(ss[8]);
    return tx->base.kind == D3DK_TEXTURE ? 1 : tx->base.kind == D3DK_CUBETEXTURE ? 2 : 3;
}

void ff_build(struct d3d_device *dv, const uint8_t (*el)[8], uint32_t nel, uint32_t prim, int with_vs, int with_ps,
              struct ff_draw *dr, struct ff_vkey *vk, struct ff_pkey *pk, struct ff_vs_u *vu, struct ff_ps_u *pu) {
    struct d3d_state *st = &dv->st;
    const uint32_t *rs = st->rs;
    if (rs[151] && !with_vs) d3d_fail("D3DRS_VERTEXBLEND %u is not supported", rs[151]);
    if (rs[167] && !with_vs) d3d_fail("D3DRS_INDEXEDVERTEXBLENDENABLE is not supported");
    if (rs[156]) d3d_fail("D3DRS_POINTSPRITEENABLE is not supported");
    if (rs[8] == 1) d3d_fail("D3DFILL_POINT is not supported");
    memset(vk, 0, sizeof *vk), memset(pk, 0, sizeof *pk), memset(vu, 0, sizeof *vu), memset(pu, 0, sizeof *pu);

    /* the vertex layout (a vertex shader's inputs are d3d_vs_inputs') */
    dr->nattr = 0;
    int have_pos = with_vs;
    uint32_t taken = 0;                                  /* slots used: a repeated usage keeps its first */
    for (uint32_t i = 0; i < nel && !with_vs; i++) {
        uint32_t stream = el[i][0] | el[i][1] << 8, off = el[i][2] | el[i][3] << 8, type = el[i][4];
        uint32_t usage = el[i][6], idx = el[i][7];
        int slot = -1;
        if ((usage == U_POSITION || usage == U_POSITIONT) && idx == 0) slot = VA_POSITION;
        else if (usage == U_NORMAL && idx == 0) slot = VA_NORMAL;
        else if (usage == U_COLOR && idx < 2) slot = VA_COLOR0 + (int)idx;
        else if (usage == U_TEXCOORD && idx < 8) slot = VA_TEX0 + (int)idx;
        if (slot < 0 || type == 17 || (taken & (1u << slot))) continue;
        taken |= 1u << slot;
        if (slot == VA_POSITION) have_pos = 1, vk->pretransformed = usage == U_POSITIONT;
        else if (slot == VA_NORMAL) vk->has_normal = 1;
        else if (slot == VA_COLOR0) vk->has_diffuse = 1;
        else if (slot == VA_COLOR1) vk->has_specular = 1;
        else vk->tex_comps[slot - VA_TEX0] = (uint8_t)type_comps(type);
        if (stream >= 16) d3d_fail("vertex element on stream %u", stream);
        struct ff_attr *a = &dr->attr[dr->nattr++];
        a->slot = (uint8_t)slot, a->decltype = (uint8_t)type, a->stream = (uint8_t)stream, a->offset = off;
    }
    if (!have_pos) d3d_fail("vertex declaration without a position");

    /* the texture stages: up to the first DISABLE. With a pixel shader the stages' combiners are unused, and
     * the fixed vertex stage writes all eight coordinate sets, each as its stage's index and transform say. */
    int n = 0;
    while (n < NTS && st->tss[n][1] != 1) n++;
    int nc = with_ps ? NTS : n;
    vk->nstages = (uint8_t)nc;
    pk->nstages = (uint8_t)(with_ps ? 0 : n);
    for (int i = 0; i < nc; i++) {
        const uint32_t *ts = st->tss[i];
        uint32_t tci = ts[11];
        vk->tci_set[i] = (uint8_t)((tci & 0xFFFF) < 8 ? tci & 0xFFFF : 0), vk->tci_mode[i] = (uint8_t)(tci >> 16);
        if (vk->tci_mode[i] > 4) d3d_fail("D3DTSS_TEXCOORDINDEX 0x%X is not supported", tci);
        uint32_t ttf = ts[24];
        vk->ttf_count[i] = (uint8_t)(ttf & 0xFF);
    }
    for (int i = 0; i < pk->nstages; i++) {
        const uint32_t *ts = st->tss[i];
        pk->colorop[i] = (uint8_t)ts[1], pk->alphaop[i] = (uint8_t)ts[4];
        pk->colorarg[i][0] = (uint8_t)ts[26], pk->colorarg[i][1] = (uint8_t)ts[2], pk->colorarg[i][2] = (uint8_t)ts[3];
        pk->alphaarg[i][0] = (uint8_t)ts[27], pk->alphaarg[i][1] = (uint8_t)ts[5], pk->alphaarg[i][2] = (uint8_t)ts[6];
        pk->resultarg[i] = (uint8_t)ts[28];
        uint32_t ttf = ts[24];
        pk->tcount[i] = (uint8_t)(ttf & 0xFF), pk->projected[i] = (ttf & 256) != 0;
        pk->textype[i] = (uint8_t)ff_bind_sampler(dv, (uint32_t)i, dr, pu);
        color4(pu->constant[i], ts[32]);
    }
    for (int i = 0; i < NTS; i++) {                     /* the bump matrices: texbem and bem read any stage's */
        const uint32_t *ts = st->tss[i];
        pu->bumpmat[i][0] = f32(ts[7]), pu->bumpmat[i][1] = f32(ts[8]), pu->bumpmat[i][2] = f32(ts[9]);
        pu->bumpmat[i][3] = f32(ts[10]);
        pu->bumpl[i][0] = f32(ts[22]), pu->bumpl[i][1] = f32(ts[23]);
    }

    /* vertex processing */
    vk->points = prim == 1;
    vk->flat = pk->flat = rs[9] == 1;
    if (!vk->pretransformed && !with_vs) {
        vk->lighting = rs[137] != 0;
        vk->normalize = rs[143] != 0, vk->localviewer = rs[142] != 0, vk->specular = rs[29] != 0;
        int cv = rs[141] != 0;
        static const int msrc[4] = { 145, 146, 147, 148 };          /* diffuse, specular, ambient, emissive */
        for (int m = 0; m < 4; m++) {
            uint32_t s = rs[msrc[m]];
            int ok = cv && ((s == 1 && vk->has_diffuse) || (s == 2 && vk->has_specular));
            vk->mat_src[m] = ok ? (uint8_t)s : 0;
        }
        if (vk->lighting)
            for (uint32_t li = 0; li < st->nlights && vk->nlights < 8; li++) {
                const struct d3d_light *l = &st->lights[li];
                if (!l->enabled) continue;
                vk->light_type[vk->nlights] = (uint8_t)l->data[0];
                struct ff_light_u *u = &vu->light[vk->nlights++];
                memcpy(u->diffuse, &l->data[1], 16), memcpy(u->specular, &l->data[5], 16);
                memcpy(u->ambient, &l->data[9], 16);
                float pos[4] = { f32(l->data[13]), f32(l->data[14]), f32(l->data[15]), 1 };
                float dir[4] = { f32(l->data[16]), f32(l->data[17]), f32(l->data[18]), 0 };
                vxm(u->position, pos, st->xform[2]);
                vxm(u->direction, dir, st->xform[2]);
                float len = sqrtf(u->direction[0] * u->direction[0] + u->direction[1] * u->direction[1] +
                                  u->direction[2] * u->direction[2]);
                if (len > 0) for (int c = 0; c < 3; c++) u->direction[c] /= len;
                u->range = f32(l->data[19]), u->falloff = f32(l->data[20]);
                u->att0 = f32(l->data[21]), u->att1 = f32(l->data[22]), u->att2 = f32(l->data[23]);
                u->cos_theta2 = cosf(f32(l->data[24]) / 2), u->cos_phi2 = cosf(f32(l->data[25]) / 2);
            }
        uint32_t cp = rs[152] & 0x3F;
        for (int c = 0; c < 6; c++)
            if (cp & (1u << c)) memcpy(vu->clip[vk->nclip++], st->clip[c], 16);
    }
    /* fog: table (per pixel) wins, then vertex, then the specular alpha */
    if (rs[28]) {
        if (rs[35]) pk->fog = (uint8_t)(1 + rs[35]);
        else if (rs[140] && !vk->pretransformed) vk->fog_vertex = (uint8_t)rs[140], vk->range_fog = rs[48] != 0, pk->fog = 1;
        else vk->fog_from_specular = 1, pk->fog = 1;
    }
    /* w-fog: a perspective projection (_34 non-zero) gives table fog in view-space units, as D3D9 hardware */
    vk->wfog = !vk->pretransformed && st->xform[3][11] != 0.0f;
    pk->specular_add = rs[29] != 0;
    pk->alpha_func = rs[15] ? (uint8_t)rs[25] : 0;
    pk->srgb_write = rs[194] != 0;

    /* uniforms */
    const float *W = st->xform[256], *V = st->xform[2], *P = st->xform[3];
    memcpy(vu->world, W, 64), memcpy(vu->view, V, 64), memcpy(vu->proj, P, 64);
    mmul(vu->wv, W, V);
    mmul(vu->wvp, vu->wv, P);
    float inv[16];
    minverse(inv, vu->wv);
    mtranspose(vu->normal_m, inv);
    for (int i = 0; i < 8; i++) memcpy(vu->tex[i], st->xform[16 + i], 64);
    const struct d3d_viewport *vp = &st->vp;
    vu->vp[0] = (float)vp->x, vu->vp[1] = (float)vp->y, vu->vp[2] = (float)vp->w, vu->vp[3] = (float)vp->h;
    vu->vpz[0] = vp->minz, vu->vpz[1] = vp->maxz, vu->vpz[2] = vp->w ? 1.0f / vp->w : 0, vu->vpz[3] = vp->h ? 1.0f / vp->h : 0;
    memcpy(vu->mat_diffuse, &st->material[0], 16), memcpy(vu->mat_ambient, &st->material[4], 16);
    memcpy(vu->mat_specular, &st->material[8], 16), memcpy(vu->mat_emissive, &st->material[12], 16);
    vu->mat_power = f32(st->material[16]);
    color4(vu->ambient, rs[139]);
    vu->fog[0] = f32(rs[36]), vu->fog[1] = f32(rs[37]), vu->fog[2] = f32(rs[38]);
    color4(pu->tfactor, rs[60]);
    color4(pu->fogcolor, rs[34]);
    pu->fog[0] = f32(rs[36]), pu->fog[1] = f32(rs[37]), pu->fog[2] = f32(rs[38]), pu->fog[3] = (float)(rs[24] & 0xFF);

    /* fixed state */
    struct ff_fixed *fx = &dr->fx;
    fx->blend = rs[27] != 0, fx->src = rs[19], fx->dst = rs[20], fx->op = rs[171];
    if (rs[206]) fx->srca = rs[207], fx->dsta = rs[208], fx->opa = rs[209];
    else fx->srca = fx->src, fx->dsta = fx->dst, fx->opa = fx->op;
    fx->wmask[0] = rs[168], fx->wmask[1] = rs[190], fx->wmask[2] = rs[191], fx->wmask[3] = rs[192];
    for (int i = 0; i < 4; i++)                          /* an X8 target keeps its alpha at one */
        if (dv->rt[i] && d3d_surf(dv->rt[i])->f->swizzle[3] == '1') fx->wmask[i] &= 7;
    fx->blend_factor = rs[193];
    fx->zenable = rs[7] != 0, fx->zwrite = rs[14] != 0, fx->zfunc = rs[23];
    fx->stencil = rs[52] != 0, fx->twosided = rs[185] != 0;
    fx->sfail = rs[53], fx->szfail = rs[54], fx->spass = rs[55], fx->sfunc = rs[56];
    fx->ccw_fail = rs[186], fx->ccw_zfail = rs[187], fx->ccw_pass = rs[188], fx->ccw_func = rs[189];
    fx->sref = rs[57], fx->smask = rs[58], fx->swmask = rs[59];
    fx->cull = rs[22], fx->fill = rs[8];
    fx->depth_bias = f32(rs[195]), fx->slope_bias = f32(rs[175]);
    fx->scissor = rs[174] != 0;
    const struct d3d_rect *sc = &st->scissor;
    fx->sc_x = sc->l > 0 ? (uint32_t)sc->l : 0, fx->sc_y = sc->t > 0 ? (uint32_t)sc->t : 0;
    fx->sc_w = sc->r > (int32_t)fx->sc_x ? (uint32_t)sc->r - fx->sc_x : 0;
    fx->sc_h = sc->b > (int32_t)fx->sc_y ? (uint32_t)sc->b - fx->sc_y : 0;
    fx->vp[0] = (float)vp->x, fx->vp[1] = (float)vp->y, fx->vp[2] = (float)vp->w, fx->vp[3] = (float)vp->h;
    fx->vp[4] = vp->minz, fx->vp[5] = vp->maxz;
    dr->vk = vk, dr->pk = pk, dr->vu = vu, dr->pu = pu;
}
