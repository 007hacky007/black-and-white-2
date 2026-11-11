/*
 * mtl.h - the Metal side of the D3D9 layer (mtl_*.m), as a C interface. Metal
 * objects cross it as opaque retained pointers (mtl_obj); mtl_release drops one.
 */
#ifndef MTL_H
#define MTL_H
#include <stdint.h>
#include "d3d9_format.h"

typedef void *mtl_obj;
struct mtl_ctx;                          /* one per device */

enum mtl_textype { MT_2D, MT_CUBE, MT_3D };
#define MT_RENDER 1u                     /* usable as a render target / depth attachment */
#define MT_MIPGEN 2u                     /* mipmaps generated on the GPU */
#define MT_OPAQUE 4u                     /* an X8 format: alpha reads (and starts) as one */

struct mtl_ctx *mtl_ctx_new(uint32_t width, uint32_t height, int vsync);   /* opens the window unless headless */
void mtl_ctx_free(struct mtl_ctx *c);
void mtl_ctx_resize(struct mtl_ctx *c, uint32_t width, uint32_t height);
void mtl_release(mtl_obj o);

/* Textures: storage format from the table; swizzle applies to sampling only. */
mtl_obj mtl_tex_new(struct mtl_ctx *c, enum mtl_store st, enum mtl_textype type, uint32_t w, uint32_t h,
                    uint32_t depth, uint32_t levels, uint32_t flags);
mtl_obj mtl_tex_view(mtl_obj tex, const char *swizzle, int srgb);           /* the sampling view */
/* Copies in command order: the GPU sees them between the draws they were made between. */
void mtl_tex_upload(struct mtl_ctx *c, mtl_obj tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                    uint32_t z, uint32_t w, uint32_t h, uint32_t d, const void *data, uint32_t pitch,
                    uint32_t slice_pitch);
void mtl_tex_download(struct mtl_ctx *c, mtl_obj tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, void *out, uint32_t pitch);   /* waits for the GPU */
void mtl_tex_copy(struct mtl_ctx *c, mtl_obj src, uint32_t sslice, uint32_t slevel, uint32_t sx, uint32_t sy,
                  mtl_obj dst, uint32_t dslice, uint32_t dlevel, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
void mtl_tex_mipgen(struct mtl_ctx *c, mtl_obj tex);
uint32_t mtl_tex_width(mtl_obj tex);
uint32_t mtl_tex_height(mtl_obj tex);

/* Render targets (a level/slice of a texture) for the next pass. */
struct mtl_target { mtl_obj tex; uint32_t slice, level; };
void mtl_set_targets(struct mtl_ctx *c, const struct mtl_target rt[4], struct mtl_target ds);
void mtl_begin_pass_if_needed(struct mtl_ctx *c);
void *mtl_encoder(struct mtl_ctx *c);    /* the open render encoder (id<MTLRenderCommandEncoder>), bridged */

/* Clear: rects already clipped to the viewport and scissor; color as D3DCOLOR. */
struct mtl_rect { uint32_t x, y, w, h; };
void mtl_clear(struct mtl_ctx *c, uint32_t flags, uint32_t color, float z, uint32_t stencil,
               const struct mtl_rect *rects, uint32_t n);

/* Present the back buffer: scaled to fit the window (or the test drawable), black bars. */
void mtl_present(struct mtl_ctx *c, mtl_obj backbuffer);
void mtl_flush(struct mtl_ctx *c);       /* commit what is recorded and wait */
uint64_t mtl_frame(struct mtl_ctx *c);   /* frames presented */

/* GPU-visible guest memory for vertex and index buffers (mtl_buffer.m). */
uint32_t mtl_heap_alloc(uint32_t size);                       /* zeroed, 256-aligned */
void mtl_heap_free(uint32_t va);
void mtl_heap_free_after(struct mtl_ctx *c, uint64_t serial, uint32_t va);   /* once that work has finished */
void mtl_heap_forget_ctx(struct mtl_ctx *c);
void *mtl_heap_buffer(uint32_t va, uint32_t *offset);         /* id<MTLBuffer> (not retained) */
uint64_t mtl_serial(struct mtl_ctx *c);                       /* the serial of the work being recorded */
int mtl_serial_done(struct mtl_ctx *c, uint64_t serial);

/* StretchRect's scaled or converting copy: a draw into dst's rect, sampling src's. */
void mtl_stretch(struct mtl_ctx *c, mtl_obj src, const char *swizzle, uint32_t sslice, uint32_t slevel,
                 const struct mtl_rect *sr, struct mtl_target dst, const struct mtl_rect *dr, int linear);

/* Occlusion queries: the samples passing between begin and end, over however many passes. */
struct mtl_occlusion { uint32_t slot[256]; uint32_t n; uint64_t serial; };
void mtl_occlusion_begin(struct mtl_ctx *c, struct mtl_occlusion *q);
void mtl_occlusion_end(struct mtl_ctx *c, struct mtl_occlusion *q);
int mtl_occlusion_result(struct mtl_ctx *c, const struct mtl_occlusion *q, uint64_t *samples);   /* 1 when done */
void mtl_commit_nowait(struct mtl_ctx *c);    /* send what is recorded (a query's FLUSH) */

/* Headless tests: Present draws into an offscreen drawable of this size. */
void mtl_set_test_drawable(uint32_t w, uint32_t h);
void mtl_read_test_drawable(uint32_t *argb);

#endif
