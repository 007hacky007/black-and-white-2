/*
 * d3d9_device.h - the device, its state and its surfaces (plan 4 Tasks 4-7).
 */
#ifndef D3D9_DEVICE_H
#define D3D9_DEVICE_H
#include <stdint.h>
#include "d3d_com.h"
#include "d3d9_format.h"
#include "mtl.h"

#define D3DPOOL_DEFAULT   0u
#define D3DPOOL_MANAGED   1u
#define D3DPOOL_SYSTEMMEM 2u
#define D3DPOOL_SCRATCH   3u

#define D3DLOCK_READONLY    0x10u
#define D3DLOCK_DISCARD     0x2000u
#define D3DLOCK_NOOVERWRITE 0x1000u
#define D3DLOCK_NOSYSLOCK   0x800u

#define NRS  256                 /* render states */
#define NSMP 20                  /* samplers 0-15, then vertex samplers 257-260 */
#define NSS  14                  /* sampler states */
#define NTS  8                   /* texture stages */
#define NTSS 33                  /* texture stage states */

/* State groups, for dirty tracking (Task 6). */
#define DIRTY_BLEND   0x001u
#define DIRTY_DS      0x002u
#define DIRTY_RASTER  0x004u
#define DIRTY_SAMPLER 0x008u
#define DIRTY_STAGE   0x010u
#define DIRTY_XFORM   0x020u
#define DIRTY_LIGHT   0x040u
#define DIRTY_INPUT   0x080u
#define DIRTY_VIEW    0x100u
#define DIRTY_TARGET  0x200u
#define DIRTY_ALL     0x3FFu

struct d3d_viewport { uint32_t x, y, w, h; float minz, maxz; };
struct d3d_rect { int32_t l, t, r, b; };

struct d3d_light { uint32_t data[26]; int enabled; int set; };      /* D3DLIGHT9 is 104 bytes */

struct d3d_state {
    uint32_t rs[NRS];
    uint32_t ss[NSMP][NSS];
    uint32_t tss[NTS][NTSS];
    float xform[512][16];        /* D3DTS: VIEW 2, PROJECTION 3, TEXTURE0-7 16-23, WORLD(n) 256+n */
    uint32_t material[17];       /* D3DMATERIAL9 is 68 bytes */
    struct d3d_light *lights;
    uint32_t nlights;
    float clip[6][4];
    struct d3d_viewport vp;
    struct d3d_rect scissor;
    uint32_t tex[NSMP];          /* guest texture objects (referenced) */
    uint32_t stream[16], stream_off[16], stream_stride[16], stream_freq[16];
    uint32_t indices;
    uint32_t fvf, vdecl;
    uint32_t vs, ps;
    float vsf[256][4], psf[224][4];
    int32_t vsi[16][4], psi[16][4];
    uint32_t vsb[16], psb[16];
    uint32_t npatch_mode, sw_vp;
    uint32_t palette_cur;
    uint32_t dirty;
};

struct d3d_device {
    struct d3d_obj base;
    uint32_t d3d9;                       /* the IDirect3D9 it came from (referenced) */
    uint32_t adapter, devtype, focus, flags;
    uint32_t pp[14];                     /* D3DPRESENT_PARAMETERS */
    uint32_t swapchain;                  /* the implicit swap chain */
    uint32_t backbuffer, auto_ds;        /* held by the device */
    uint32_t rt[4], ds;                  /* bound (referenced) */
    struct d3d_state st;
    int in_scene;
    uint32_t default_pool_live;          /* guest-made DEFAULT resources: Reset needs 0 */
    uint16_t gamma[768];
    uint32_t cursor_x, cursor_y, cursor_shown, dialog_mode;
    uint32_t recording;                  /* the state block being recorded, or 0 (Task 7) */
    struct mtl_ctx *mtl;
};

struct d3d_box { uint32_t l, t, r, b, front, back; };

struct d3d_texture {
    struct d3d_obj base;
    uint32_t device;
    uint32_t fmt, usage, pool, w, h, d, levels, faces;
    const struct d3d_format *f;
    mtl_obj tex, view;                   /* the Metal texture and its sampling view (NULL in system memory) */
    mtl_obj srgb_view;                   /* made when SRGBTEXTURE first samples it */
    uint32_t mtl_levels;                 /* AUTOGENMIPMAP: the full chain, of which the guest sees 1 */
    int autogen;
    uint32_t *mem;                       /* per subresource (face * levels + level): guest staging, or 0 */
    uint32_t *child;                     /* per subresource: its surface or volume object, or 0 */
    int *locked, *lock_ro;
    struct d3d_box *lockbox;
    uint32_t lod, priority, autogen_filter;
    int counts_for_reset;
};

struct d3d_buffer {
    struct d3d_obj base;
    uint32_t device;
    uint32_t size, usage, fvf_fmt, pool;
    uint32_t mem;                        /* GPU-visible guest block (mtl_heap) */
    uint64_t used_serial;                /* the work that last read it */
    struct mtl_ctx *used_ctx;
    uint32_t locks;
    int counts_for_reset;
};

struct d3d_surface {
    struct d3d_obj base;
    uint32_t device;                     /* guest device */
    uint32_t container;                  /* the texture or swap chain it belongs to (0: the device) */
    uint32_t fmt, type_usage, pool, w, h, ms, msq;
    const struct d3d_format *f;
    mtl_obj tex;                         /* NULL for SYSTEMMEM/SCRATCH */
    uint32_t slice, level;
    uint32_t mem;                        /* the guest staging copy, D3D layout (0 until first lock) */
    uint32_t pitch, mem_size;
    int locked, lock_ro;
    struct d3d_rect lock;
    int counts_for_reset;
    int owns_tex;
    struct d3d_texture *parent;          /* a texture's level: its staging is the texture's */
    uint32_t sub;
};

/* d3d9_device.c */
struct d3d_device *d3d_dev(uint32_t obj);              /* the host device of a guest device */
struct d3d_state *d3d_cur(void);                       /* the device last called */
extern struct d3d_device *g_d3d_cur;
uint32_t d3d9_create_device(uint32_t d3d9, uint32_t adapter, uint32_t type, uint32_t focus, uint32_t flags,
                            uint32_t pp, uint32_t *out);
void d3d_bind(uint32_t *slot, uint32_t obj);           /* replace a referenced binding */
void d3d_apply_targets(struct d3d_device *d);          /* the bound targets to the Metal pass */

/* d3d9_state.c */
void d3d_state_defaults(struct d3d_state *st, int auto_ds, uint32_t rt_w, uint32_t rt_h);
void d3d_state_free(struct d3d_state *st);
void d3d9_state_register(void);

/* d3d9_surface.c */
uint32_t d3d_surface_new(uint32_t device, uint32_t container, uint32_t fmt, uint32_t usage, uint32_t pool,
                         uint32_t w, uint32_t h, mtl_obj tex, uint32_t slice, uint32_t level, int counts_for_reset);
struct d3d_surface *d3d_surf(uint32_t obj);
void d3d_surface_upload(struct d3d_surface *s, const struct d3d_rect *r);      /* staging -> GPU */
void d3d_surface_download(struct d3d_surface *s);                              /* GPU -> staging */
uint32_t d3d_surface_mem(struct d3d_surface *s);                               /* the staging copy, made on first use */
void d3d9_surface_register(void);

/* d3d9_texture.c */
struct d3d_texture *d3d_tex(uint32_t obj);             /* any of the three texture kinds */
uint32_t d3d_tex_level_w(const struct d3d_texture *t, uint32_t level);
uint32_t d3d_tex_level_h(const struct d3d_texture *t, uint32_t level);
uint32_t d3d_tex_level_d(const struct d3d_texture *t, uint32_t level);
uint32_t d3d_tex_mem(struct d3d_texture *t, uint32_t sub);              /* the staging copy, made on first use */
void d3d_tex_upload(struct d3d_texture *t, uint32_t sub, const struct d3d_box *b);
mtl_obj d3d_tex_srgb_view(struct d3d_texture *t);
void d3d9_texture_register(void);

/* d3d9_buffer.c */
struct d3d_buffer *d3d_buf(uint32_t obj);              /* a vertex or index buffer */
void *d3d_buffer_for_draw(uint32_t obj, uint32_t *offset);             /* id<MTLBuffer>; marks it in use */
void d3d9_buffer_register(void);

/* d3d9_draw.c, ff_key.c */
struct ff_draw; struct ff_vkey; struct ff_pkey; struct ff_vs_u; struct ff_ps_u;
void ff_build(struct d3d_device *dv, const uint8_t (*el)[8], uint32_t nel, uint32_t prim, struct ff_draw *dr,
              struct ff_vkey *vk, struct ff_pkey *pk, struct ff_vs_u *vu, struct ff_ps_u *pu);
void d3d9_draw_register(void);

/* d3d9_copy.c */
void d3d9_copy_register(void);

/* d3d9_convert.c: between the D3D layout and the GPU storage */
uint32_t d3d_storage_pitch(const struct d3d_format *f, uint32_t w);
void d3d_to_storage(const struct d3d_format *f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t dpitch,
                    uint32_t w, uint32_t rows);
void d3d_from_storage(const struct d3d_format *f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t dpitch,
                      uint32_t w, uint32_t rows);

#endif
