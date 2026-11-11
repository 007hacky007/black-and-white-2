/*
 * ff.h - the fixed-function pipeline (plan 4 Task 6): the state key the
 * shaders are generated from, the uniforms they read, and a draw as the Metal
 * side records it.
 */
#ifndef FF_H
#define FF_H
#include <stdint.h>
#include "mtl.h"

/* Vertex attribute slots the generated vertex function reads. */
enum { VA_POSITION, VA_BLENDWEIGHT, VA_BLENDINDICES, VA_NORMAL, VA_PSIZE, VA_COLOR0, VA_COLOR1, VA_TEX0,
       VA_FOG = VA_TEX0 + 8, VA_COUNT };
#define FF_STREAM_BUFFER0 8           /* Metal buffer index of stream 0 */

struct ff_vkey {
    uint8_t pretransformed, has_normal, has_diffuse, has_specular, lighting, normalize, localviewer, colorvertex;
    uint8_t specular, fog_vertex, range_fog, fog_from_specular, nclip, points, flat, pad0;
    uint8_t mat_src[4];               /* diffuse, specular, ambient, emissive: 0 material, 1 color1, 2 color2 */
    uint8_t nlights, light_type[8];   /* the enabled lights, in index order: 1 point, 2 spot, 3 directional */
    uint8_t tex_comps[8];             /* components of each vertex texcoord set (0: absent) */
    uint8_t tci_set[8], tci_mode[8];  /* per stage: TEXCOORDINDEX's set and its generation mode (>> 16) */
    uint8_t ttf_count[8];             /* per stage: texture transform count (0: off) */
    uint8_t nstages, pad1[3];
};

struct ff_pkey {
    uint8_t colorop[8], colorarg[8][3], alphaop[8], alphaarg[8][3], resultarg[8];
    uint8_t textype[8];               /* 0 none, 1 2D, 2 cube, 3 volume */
    uint8_t tcount[8], projected[8];  /* how many texcoord components the stage reads; projected divide */
    uint8_t alpha_func;               /* D3DCMP, 0 when the test is off */
    uint8_t fog;                      /* 0 none, 1 vertex factor, 2 table EXP, 3 EXP2, 4 LINEAR */
    uint8_t specular_add, flat, srgb_write, nstages, pad[2];
};

/* Uniforms: laid out the same way in ff_msl.c's generated MSL. */
struct ff_light_u {
    float diffuse[4], specular[4], ambient[4];
    float position[4];                /* view space */
    float direction[4];               /* view space, toward the light's aim */
    float range, falloff, att0, att1;
    float att2, cos_theta2, cos_phi2, pad;
};
struct ff_vs_u {
    float wv[16], proj[16], wvp[16], normal_m[16], view[16], world[16];
    float tex[8][16];
    float vp[4];                      /* viewport x, y, w, h */
    float vpz[4];                     /* minz, maxz, 1/w, 1/h (the half-pixel offset) */
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
    float ambient[4];
    float fog[4];                     /* start, end, density, 0 */
    float clip[6][4];                 /* world space */
    float mat_power, pad[3];
    struct ff_light_u light[8];
};
struct ff_ps_u {
    float tfactor[4];
    float constant[8][4];
    float bumpmat[8][4];              /* 00, 01, 10, 11 */
    float bumpl[8][4];                /* lscale, loffset */
    float lodbias[8][4];
    float fogcolor[4];
    float fog[4];                     /* start, end, density, alpha ref (0-1) */
};

/* What a draw sets beyond the shaders: fixed state numbers, as D3D gives them. */
struct ff_fixed {
    uint32_t blend, src, dst, op, srca, dsta, opa, wmask[4], blend_factor;
    uint32_t zenable, zwrite, zfunc;
    uint32_t stencil, twosided, sfail, szfail, spass, sfunc, ccw_fail, ccw_zfail, ccw_pass, ccw_func;
    uint32_t sref, smask, swmask;
    uint32_t cull, fill;
    float depth_bias, slope_bias;
    uint32_t scissor, sc_x, sc_y, sc_w, sc_h;
    float vp[6];                      /* x, y, w, h, minz, maxz */
};

struct ff_sampler { uint32_t u, v, w, mag, min, mip, aniso, maxmip, border; };

struct ff_attr { uint8_t slot, decltype, stream, pad; uint32_t offset; };

struct ff_draw {
    const struct ff_vkey *vk;
    const struct ff_pkey *pk;
    const struct ff_vs_u *vu;
    const struct ff_ps_u *pu;
    struct ff_fixed fx;
    uint32_t nattr;
    struct ff_attr attr[VA_COUNT];
    void *stream_buf[16];             /* id<MTLBuffer> per stream, or NULL */
    uint32_t stream_off[16], stream_stride[16];
    mtl_obj tex[8];                   /* sampling views */
    struct ff_sampler smp[8];
    uint32_t prim;                    /* D3DPRIMITIVETYPE (fans already lists) */
    uint32_t count, start;            /* vertices, or indices when indexed */
    int32_t base_vertex;
    int indexed, index32;
    void *ibuf;
    uint32_t ioff;
};

/* mtl_pipeline.m */
void mtl_draw(struct mtl_ctx *c, const struct ff_draw *d);
struct mtl_stats { uint64_t libraries, pipelines, archive_hits, archive_misses; };
void mtl_get_stats(struct mtl_stats *s);
void mtl_archive_open(const char *path);         /* load (or start) the pipeline archive */
void mtl_archive_save(void);
void mtl_archive_require_hits(int on);           /* tests: pipelines must come from the archive */
void mtl_pipeline_cache_drop(void);              /* tests: forget the in-memory libraries and pipelines */

/* ff_msl.c: the MSL for a key pair; malloc'd, NUL-terminated. */
char *ff_msl(const struct ff_vkey *vk, const struct ff_pkey *pk);

#endif
