/*
 * mtl_internal.h - shared between the mtl_*.m files only.
 */
#ifndef MTL_INTERNAL_H
#define MTL_INTERNAL_H
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "mtl.h"

@interface MtlCtx : NSObject {
@public
    struct mtl_target _rt[4], _ds;      /* the targets of the next (or open) pass */
}
@property(strong) id<MTLDevice> dev;
@property(strong) id<MTLCommandQueue> queue;
@property(strong) id<MTLCommandBuffer> cb;
@property(strong) id<MTLRenderCommandEncoder> enc;
@property(strong) id<MTLBlitCommandEncoder> blit;
@property(strong) id<MTLLibrary> util;
@property(strong) id<MTLRenderPipelineState> presentPso;
@property(strong) id<MTLSamplerState> linear;
@property(strong) NSMutableDictionary *psos;
@property(strong) NSMutableDictionary *dss;
@property(strong) dispatch_semaphore_t inflight;
@property(strong) NSWindow *window;
@property(strong) CAMetalLayer *layer;
@property uint32_t passW, passH;
@property uint64_t submitted;                 /* serial of the last committed command buffer */
@property(atomic) uint64_t completed;         /* serial of the last finished one */
@property uint64_t frames, encGen;
@property uint64_t staged;                    /* staging bytes the open command buffer holds */
@property int vsync;
@property(strong) id<MTLBuffer> vis;             /* occlusion counts, 8 bytes per slot */
@property uint32_t visNext;
@property struct mtl_occlusion *occl;            /* the query counting now, or NULL */
@end

MTLPixelFormat mtl_pixfmt(enum mtl_store st, int srgb);
int mtl_is_depth(MTLPixelFormat f);
int mtl_has_stencil(MTLPixelFormat f);
void mtl_end_encoders(MtlCtx *c);
id<MTLBlitCommandEncoder> mtl_blit(MtlCtx *c);
NSString *mtl_target_key(MtlCtx *c);
void mtl_commit(MtlCtx *c, int wait);         /* commit the open command buffer (serials, frees) */
void mtl_heap_collect(void);                  /* free blocks whose last use has finished */
void mtl_pipeline_targets(MtlCtx *c, MTLRenderPipelineDescriptor *pd);

#endif
