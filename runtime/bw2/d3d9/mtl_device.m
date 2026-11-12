/*
 * mtl_device.m - the Metal context behind each D3D9 device (plan 4 Task 4):
 * device, queue, the frame's command buffer, the window and CAMetalLayer,
 * clears, copies in command order, readback and Present. Everything the
 * guest asks for is recorded into one command buffer per frame, in the order
 * asked; a readback commits and waits.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "mtl.h"
#include "mtl_internal.h"

#define VIS_SLOTS 16384u                 /* occlusion counting slots */

static NSString *const kUtilSource = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut fs_tri(uint vid [[vertex_id]], constant float &z [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  VOut o; o.pos = float4(p * 2.0 - 1.0, z, 1.0); o.uv = float2(p.x, 1.0 - p.y); return o;\n"
    "}\n"
    "struct Uv { float4 r; float lod; };\n"
    "vertex VOut copy_vert(uint vid [[vertex_id]], constant Uv &u [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  o.uv = u.r.xy + float2(p.x, 1.0 - p.y) * u.r.zw; return o;\n"
    "}\n"
    "fragment float4 copy_frag(VOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                          sampler s [[sampler(0)]], constant Uv &u [[buffer(0)]]) {\n"
    "  return t.sample(s, in.uv, level(u.lod));\n"
    "}\n"
    "fragment float4 present_frag(VOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
    "                             sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, in.uv).rgb, 1.0);\n"
    "}\n";

MTLPixelFormat mtl_pixfmt(enum mtl_store st, int srgb) {
    switch (st) {
    case ST_BGRA8: return srgb ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
    case ST_RGBA8: return srgb ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
    case ST_R8: return MTLPixelFormatR8Unorm;
    case ST_A8: return MTLPixelFormatA8Unorm;
    case ST_RG8: return MTLPixelFormatRG8Unorm;
    case ST_R16: return MTLPixelFormatR16Unorm;
    case ST_RG8S: return MTLPixelFormatRG8Snorm;
    case ST_RGBA8S: return MTLPixelFormatRGBA8Snorm;
    case ST_RG16: return MTLPixelFormatRG16Unorm;
    case ST_RGBA16: return MTLPixelFormatRGBA16Unorm;
    case ST_R16F: return MTLPixelFormatR16Float;
    case ST_RG16F: return MTLPixelFormatRG16Float;
    case ST_RGBA16F: return MTLPixelFormatRGBA16Float;
    case ST_R32F: return MTLPixelFormatR32Float;
    case ST_RG32F: return MTLPixelFormatRG32Float;
    case ST_RGBA32F: return MTLPixelFormatRGBA32Float;
    case ST_BC1: return srgb ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA;
    case ST_BC2: return srgb ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA;
    case ST_BC3: return srgb ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA;
    case ST_D16: return MTLPixelFormatDepth16Unorm;
    case ST_D32S8: return MTLPixelFormatDepth32Float_Stencil8;
    case ST_D32: return MTLPixelFormatDepth32Float;
    default: return MTLPixelFormatInvalid;
    }
}
int mtl_is_depth(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth32Float;
}
int mtl_has_stencil(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8; }
static int is_bc(MTLPixelFormat f) {
    return f == MTLPixelFormatBC1_RGBA || f == MTLPixelFormatBC2_RGBA || f == MTLPixelFormatBC3_RGBA ||
           f == MTLPixelFormatBC1_RGBA_sRGB || f == MTLPixelFormatBC2_RGBA_sRGB || f == MTLPixelFormatBC3_RGBA_sRGB;
}
/* Bytes per pixel (per 4x4 block for BC). */
static uint32_t fmt_bytes(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatA8Unorm: return 1;
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatR16Unorm:
    case MTLPixelFormatR16Float: case MTLPixelFormatDepth16Unorm: return 2;
    case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Float: return 8;
    case MTLPixelFormatRGBA32Float: return 16;
    case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: return 8;
    case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB: return 16;
    default: return 4;
    }
}

static MtlCtx *ctx(struct mtl_ctx *c) { return (__bridge MtlCtx *)(void *)c; }

@implementation MtlCtx
@end

/* ---- test drawable (headless) ---- */
static id<MTLTexture> g_test_drawable;
static uint32_t g_test_w, g_test_h;
static id<MTLDevice> g_device;

static id<MTLDevice> device(void) {
    if (!g_device) g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) { fprintf(stderr, "[bw2] FATAL: no Metal device\n"); abort(); }
    return g_device;
}

void mtl_set_test_drawable(uint32_t w, uint32_t h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    g_test_drawable = [device() newTextureWithDescriptor:d];
    g_test_w = w, g_test_h = h;
}
void mtl_read_test_drawable(uint32_t *argb) {
    if (!g_test_drawable) return;
    [g_test_drawable getBytes:argb bytesPerRow:g_test_w * 4 fromRegion:MTLRegionMake2D(0, 0, g_test_w, g_test_h)
                  mipmapLevel:0];
}

/* ---- encoders ---- */
void mtl_end_encoders(MtlCtx *c) {
    if (c.enc) { [c.enc endEncoding]; c.enc = nil; c.encGen++; }
    if (c.blit) { [c.blit endEncoding]; c.blit = nil; }
}
static id<MTLCommandBuffer> cmdbuf(MtlCtx *c) {
    if (!c.cb) c.cb = [c.queue commandBuffer];
    return c.cb;
}
id<MTLBlitCommandEncoder> mtl_blit(MtlCtx *c) {
    if (c.enc) { [c.enc endEncoding]; c.enc = nil; c.encGen++; }
    if (!c.blit) c.blit = [cmdbuf(c) blitCommandEncoder];
    return c.blit;
}
void mtl_commit(MtlCtx *c, int wait) {
    mtl_end_encoders(c);
    id<MTLCommandBuffer> cb = c.cb;
    if (!cb) { mtl_heap_collect(); return; }
    uint64_t serial = ++c.submitted;
    __weak MtlCtx *weak = c;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        if (b.error) fprintf(stderr, "[bw2] metal: command buffer failed: %s\n", b.error.description.UTF8String);
        MtlCtx *s = weak;
        if (s && s.completed < serial) s.completed = serial;
    }];
    [cb commit];
    if (wait) [cb waitUntilCompleted];
    c.cb = nil;
    mtl_heap_collect();
}
static void commit_wait(MtlCtx *c) { mtl_commit(c, 1); }
void mtl_flush(struct mtl_ctx *cc) { commit_wait(ctx(cc)); }

/* ---- context ---- */
static void make_window(MtlCtx *c, uint32_t w, uint32_t h) {
    bw2_main_sync(^{
        NSRect r = NSMakeRect(0, 0, w, h);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"Black & White 2";
        win.releasedWhenClosed = NO;
        [win center];
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = c.dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.contentsScale = win.backingScaleFactor;
        layer.drawableSize = CGSizeMake(w * win.backingScaleFactor, h * win.backingScaleFactor);
        layer.displaySyncEnabled = c.vsync ? YES : NO;
        win.contentView.wantsLayer = YES;
        win.contentView.layer = layer;
        [win makeKeyAndOrderFront:nil];
        c.window = win;
        c.layer = layer;
    });
}

struct mtl_ctx *mtl_ctx_new(uint32_t width, uint32_t height, int vsync) {
    MtlCtx *c = [MtlCtx new];
    c.dev = device();
    c.queue = [c.dev newCommandQueue];
    c.inflight = dispatch_semaphore_create(3);
    c.vsync = vsync;
    c.psos = [NSMutableDictionary dictionary];
    c.vis = [c.dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared];
    c.dss = [NSMutableDictionary dictionary];
    NSError *err = nil;
    c.util = [c.dev newLibraryWithSource:kUtilSource options:nil error:&err];
    if (!c.util) { fprintf(stderr, "[bw2] FATAL: metal util library: %s\n", err.description.UTF8String); abort(); }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [c.util newFunctionWithName:@"fs_tri"];
    pd.fragmentFunction = [c.util newFunctionWithName:@"present_frag"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    c.presentPso = [c.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!c.presentPso) { fprintf(stderr, "[bw2] FATAL: present pipeline: %s\n", err.description.UTF8String); abort(); }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    c.linear = [c.dev newSamplerStateWithDescriptor:sd];
    if (!bw2_headless()) make_window(c, width, height);
    return (__bridge_retained struct mtl_ctx *)c;
}

void mtl_ctx_free(struct mtl_ctx *cc) {
    MtlCtx *c = (__bridge_transfer MtlCtx *)(void *)cc;
    commit_wait(c);
    NSWindow *w = c.window;
    if (w) bw2_main_sync(^{ [w orderOut:nil]; [w close]; });
    c.window = nil;
}

void mtl_ctx_resize(struct mtl_ctx *cc, uint32_t width, uint32_t height) {
    MtlCtx *c = ctx(cc);
    NSWindow *w = c.window;
    CAMetalLayer *layer = c.layer;
    if (!w) return;
    bw2_main_sync(^{
        [w setContentSize:NSMakeSize(width, height)];
        layer.drawableSize = CGSizeMake(width * w.backingScaleFactor, height * w.backingScaleFactor);
    });
}

void mtl_release(mtl_obj o) { if (o) CFRelease(o); }

/* ---- textures ---- */
uint32_t mtl_tex_width(mtl_obj t) { return (uint32_t)((__bridge id<MTLTexture>)t).width; }
uint32_t mtl_tex_height(mtl_obj t) { return (uint32_t)((__bridge id<MTLTexture>)t).height; }

static void zero_texture(MtlCtx *c, id<MTLTexture> t, int opaque) {
    uint32_t slices = t.textureType == MTLTextureTypeCube ? 6 : 1;
    if (t.usage & MTLTextureUsageRenderTarget) {
        mtl_end_encoders(c);
        for (uint32_t s = 0; s < slices; s++)
            for (uint32_t l = 0; l < t.mipmapLevelCount; l++) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                if (mtl_is_depth(t.pixelFormat)) {
                    rp.depthAttachment.texture = t, rp.depthAttachment.slice = s, rp.depthAttachment.level = l;
                    rp.depthAttachment.loadAction = MTLLoadActionClear, rp.depthAttachment.clearDepth = 0;
                    rp.depthAttachment.storeAction = MTLStoreActionStore;
                    if (mtl_has_stencil(t.pixelFormat)) {
                        rp.stencilAttachment.texture = t, rp.stencilAttachment.slice = s, rp.stencilAttachment.level = l;
                        rp.stencilAttachment.loadAction = MTLLoadActionClear, rp.stencilAttachment.clearStencil = 0;
                        rp.stencilAttachment.storeAction = MTLStoreActionStore;
                    }
                } else {
                    rp.colorAttachments[0].texture = t, rp.colorAttachments[0].slice = s, rp.colorAttachments[0].level = l;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, opaque ? 1 : 0);
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                [[cmdbuf(c) renderCommandEncoderWithDescriptor:rp] endEncoding];
            }
        return;
    }
    uint32_t bpp = fmt_bytes(t.pixelFormat), bw = is_bc(t.pixelFormat) ? (uint32_t)(t.width + 3) / 4 : (uint32_t)t.width;
    uint32_t bh = is_bc(t.pixelFormat) ? (uint32_t)(t.height + 3) / 4 : (uint32_t)t.height;
    NSUInteger len = (NSUInteger)bw * bpp * bh * t.depth;
    id<MTLBuffer> z = [c.dev newBufferWithLength:len ? len : 16 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = mtl_blit(c);
    for (uint32_t s = 0; s < slices; s++)
        for (uint32_t l = 0; l < t.mipmapLevelCount; l++) {
            uint32_t w = MAX(1u, (uint32_t)t.width >> l), h = MAX(1u, (uint32_t)t.height >> l);
            uint32_t d = t.textureType == MTLTextureType3D ? MAX(1u, (uint32_t)t.depth >> l) : 1;
            uint32_t rowb = (is_bc(t.pixelFormat) ? (w + 3) / 4 : w) * bpp;
            uint32_t rows = is_bc(t.pixelFormat) ? (h + 3) / 4 : h;
            [b copyFromBuffer:z sourceOffset:0 sourceBytesPerRow:rowb sourceBytesPerImage:rowb * rows
                   sourceSize:MTLSizeMake(w, h, d) toTexture:t destinationSlice:s destinationLevel:l
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}

mtl_obj mtl_tex_new(struct mtl_ctx *cc, enum mtl_store st, enum mtl_textype type, uint32_t w, uint32_t h,
                    uint32_t depth, uint32_t levels, uint32_t flags) {
    MtlCtx *c = ctx(cc);
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = type == MT_CUBE ? MTLTextureTypeCube : type == MT_3D ? MTLTextureType3D : MTLTextureType2D;
    d.pixelFormat = mtl_pixfmt(st, 0);
    d.width = w, d.height = type == MT_CUBE ? w : h, d.depth = type == MT_3D ? depth : 1;
    d.mipmapLevelCount = levels ? levels : 1;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (flags & (MT_RENDER | MT_MIPGEN)) d.usage |= MTLTextureUsageRenderTarget;
    if (mtl_is_depth(d.pixelFormat)) d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [c.dev newTextureWithDescriptor:d];
    if (!t) { fprintf(stderr, "[bw2] FATAL: metal: no texture %ux%ux%u fmt %d\n", w, h, depth, (int)st); abort(); }
    zero_texture(c, t, (flags & MT_OPAQUE) != 0);
    return (__bridge_retained mtl_obj)t;
}

mtl_obj mtl_tex_view(mtl_obj tex, const char *swz, int srgb) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
    MTLTextureSwizzle map[4] = { MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                 MTLTextureSwizzleAlpha };
    for (int i = 0; swz && swz[0] && i < 4; i++) {
        switch (swz[i]) {
        case 'r': map[i] = MTLTextureSwizzleRed; break;
        case 'g': map[i] = MTLTextureSwizzleGreen; break;
        case 'b': map[i] = MTLTextureSwizzleBlue; break;
        case 'a': map[i] = MTLTextureSwizzleAlpha; break;
        case '0': map[i] = MTLTextureSwizzleZero; break;
        default: map[i] = MTLTextureSwizzleOne; break;
        }
    }
    MTLPixelFormat pf = t.pixelFormat;
    if (srgb) {
        if (pf == MTLPixelFormatBGRA8Unorm) pf = MTLPixelFormatBGRA8Unorm_sRGB;
        else if (pf == MTLPixelFormatRGBA8Unorm) pf = MTLPixelFormatRGBA8Unorm_sRGB;
        else if (pf == MTLPixelFormatBC1_RGBA) pf = MTLPixelFormatBC1_RGBA_sRGB;
        else if (pf == MTLPixelFormatBC2_RGBA) pf = MTLPixelFormatBC2_RGBA_sRGB;
        else if (pf == MTLPixelFormatBC3_RGBA) pf = MTLPixelFormatBC3_RGBA_sRGB;
    }
    NSUInteger slices = t.textureType == MTLTextureTypeCube ? 6 : 1;
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:pf textureType:t.textureType
                                                 levels:NSMakeRange(0, t.mipmapLevelCount) slices:NSMakeRange(0, slices)
                                                swizzle:MTLTextureSwizzleChannelsMake(map[0], map[1], map[2], map[3])];
    return (__bridge_retained mtl_obj)v;
}

void mtl_tex_upload(struct mtl_ctx *cc, mtl_obj tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                    uint32_t z, uint32_t w, uint32_t h, uint32_t d, const void *data, uint32_t pitch,
                    uint32_t slice_pitch) {
    MtlCtx *c = ctx(cc);
    id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
    uint32_t rows = is_bc(t.pixelFormat) ? (h + 3) / 4 : h, dd = d ? d : 1;
    if (!slice_pitch) slice_pitch = pitch * rows;
    /* Read only the bytes the box covers: a box that is not the whole level starts inside the
     * caller's memory, so a whole slice from there would read past its end. */
    NSUInteger rowb = (NSUInteger)(is_bc(t.pixelFormat) ? (w + 3) / 4 : w) * fmt_bytes(t.pixelFormat);
    NSUInteger span = (NSUInteger)slice_pitch * (dd - 1) + (NSUInteger)pitch * (rows - 1) + rowb;
    id<MTLBuffer> b = [c.dev newBufferWithLength:(NSUInteger)slice_pitch * dd options:MTLResourceStorageModeShared];
    memcpy(b.contents, data, span);
    [mtl_blit(c) copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:slice_pitch
                     sourceSize:MTLSizeMake(w, h, d ? d : 1) toTexture:t destinationSlice:slice
               destinationLevel:level destinationOrigin:MTLOriginMake(x, y, z)];
}

void mtl_tex_download(struct mtl_ctx *cc, mtl_obj tex, uint32_t slice, uint32_t level, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, void *out, uint32_t pitch) {
    MtlCtx *c = ctx(cc);
    id<MTLTexture> t = (__bridge id<MTLTexture>)tex;
    uint32_t rows = is_bc(t.pixelFormat) ? (h + 3) / 4 : h;
    id<MTLBuffer> b = [c.dev newBufferWithLength:(NSUInteger)pitch * rows options:MTLResourceStorageModeShared];
    [mtl_blit(c) copyFromTexture:t sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(x, y, 0)
                      sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch
            destinationBytesPerImage:pitch * rows];
    commit_wait(c);
    memcpy(out, b.contents, (size_t)pitch * rows);
}

void mtl_tex_copy(struct mtl_ctx *cc, mtl_obj src, uint32_t sslice, uint32_t slevel, uint32_t sx, uint32_t sy,
                  mtl_obj dst, uint32_t dslice, uint32_t dlevel, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    MtlCtx *c = ctx(cc);
    [mtl_blit(c) copyFromTexture:(__bridge id<MTLTexture>)src sourceSlice:sslice sourceLevel:slevel
                    sourceOrigin:MTLOriginMake(sx, sy, 0) sourceSize:MTLSizeMake(w, h, 1)
                       toTexture:(__bridge id<MTLTexture>)dst destinationSlice:dslice destinationLevel:dlevel
               destinationOrigin:MTLOriginMake(dx, dy, 0)];
}

void mtl_tex_mipgen(struct mtl_ctx *cc, mtl_obj tex) {
    [mtl_blit(ctx(cc)) generateMipmapsForTexture:(__bridge id<MTLTexture>)tex];
}

/* ---- render passes ---- */
static int same_target(struct mtl_target a, struct mtl_target b) {
    return a.tex == b.tex && a.slice == b.slice && a.level == b.level;
}
void mtl_set_targets(struct mtl_ctx *cc, const struct mtl_target rt[4], struct mtl_target ds) {
    MtlCtx *c = ctx(cc);
    int same = same_target(c->_ds, ds);
    for (int i = 0; i < 4; i++) same &= same_target(c->_rt[i], rt[i]);
    if (same) return;
    if (c.enc) { [c.enc endEncoding]; c.enc = nil; c.encGen++; }
    for (int i = 0; i < 4; i++) c->_rt[i] = rt[i];
    c->_ds = ds;
}

static void occl_count(MtlCtx *c) {
    struct mtl_occlusion *q = c.occl;
    if (q->n == sizeof q->slot / sizeof q->slot[0]) {
        fprintf(stderr, "[bw2] FATAL: d3d9: an occlusion query spans more than 256 passes\n");
        abort();
    }
    uint32_t slot = c.visNext++ % VIS_SLOTS;
    ((uint64_t *)c.vis.contents)[slot] = 0;
    q->slot[q->n++] = slot;
    [c.enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:slot * 8];
}
void mtl_occlusion_begin(struct mtl_ctx *cc, struct mtl_occlusion *q) {
    MtlCtx *c = ctx(cc);
    q->n = 0, q->serial = 0;
    c.occl = q;
    if (c.enc) occl_count(c);
}
void mtl_occlusion_end(struct mtl_ctx *cc, struct mtl_occlusion *q) {
    MtlCtx *c = ctx(cc);
    if (c.occl == q) {
        if (c.enc) [c.enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        c.occl = NULL;
    }
    q->serial = c.submitted + 1;
}
int mtl_occlusion_result(struct mtl_ctx *cc, const struct mtl_occlusion *q, uint64_t *samples) {
    MtlCtx *c = ctx(cc);
    if (c.completed < q->serial) return 0;
    uint64_t n = 0;
    for (uint32_t i = 0; i < q->n; i++) n += ((uint64_t *)c.vis.contents)[q->slot[i]];
    *samples = n;
    return 1;
}
void mtl_commit_nowait(struct mtl_ctx *cc) {
    MtlCtx *c = ctx(cc);
    cmdbuf(c);                                     /* even with nothing recorded, so the serial advances */
    mtl_commit(c, 0);
}

void mtl_begin_pass_if_needed(struct mtl_ctx *cc) {
    MtlCtx *c = ctx(cc);
    if (c.enc) return;
    if (c.blit) { [c.blit endEncoding]; c.blit = nil; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t w = 0, h = 0;
    for (int i = 0; i < 4; i++) {
        if (!c->_rt[i].tex) continue;
        id<MTLTexture> t = (__bridge id<MTLTexture>)c->_rt[i].tex;
        rp.colorAttachments[i].texture = t;
        rp.colorAttachments[i].slice = c->_rt[i].slice;
        rp.colorAttachments[i].level = c->_rt[i].level;
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (!w) w = MAX(1u, (uint32_t)t.width >> c->_rt[i].level), h = MAX(1u, (uint32_t)t.height >> c->_rt[i].level);
    }
    if (c->_ds.tex) {
        id<MTLTexture> t = (__bridge id<MTLTexture>)c->_ds.tex;
        rp.depthAttachment.texture = t;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (mtl_has_stencil(t.pixelFormat)) {
            rp.stencilAttachment.texture = t;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!w) w = (uint32_t)t.width, h = (uint32_t)t.height;
        else {
            /* D3D allows a depth surface larger than the target; Metal wants the render area inside both */
            w = MIN(w, (uint32_t)t.width), h = MIN(h, (uint32_t)t.height);
        }
    }
    rp.renderTargetWidth = w, rp.renderTargetHeight = h;
    rp.visibilityResultBuffer = c.vis;
    c.passW = w, c.passH = h;
    c.enc = [cmdbuf(c) renderCommandEncoderWithDescriptor:rp];
    c.encGen++;
    if (c.occl) occl_count(c);                     /* a query spans passes: each pass counts into its own slot */
}
void *mtl_encoder(struct mtl_ctx *cc) { return (__bridge void *)ctx(cc).enc; }

/* The formats of the pass's attachments, as a pipeline key prefix. */
NSString *mtl_target_key(MtlCtx *c) {
    NSMutableString *k = [NSMutableString string];
    for (int i = 0; i < 4; i++)
        [k appendFormat:@"%lu,", c->_rt[i].tex ? (unsigned long)((__bridge id<MTLTexture>)c->_rt[i].tex).pixelFormat : 0ul];
    [k appendFormat:@"%lu", c->_ds.tex ? (unsigned long)((__bridge id<MTLTexture>)c->_ds.tex).pixelFormat : 0ul];
    return k;
}
void mtl_pipeline_targets(MtlCtx *c, MTLRenderPipelineDescriptor *pd) {
    for (int i = 0; i < 4; i++)
        pd.colorAttachments[i].pixelFormat =
            c->_rt[i].tex ? ((__bridge id<MTLTexture>)c->_rt[i].tex).pixelFormat : MTLPixelFormatInvalid;
    if (c->_ds.tex) {
        MTLPixelFormat f = ((__bridge id<MTLTexture>)c->_ds.tex).pixelFormat;
        pd.depthAttachmentPixelFormat = f;
        if (mtl_has_stencil(f)) pd.stencilAttachmentPixelFormat = f;
    }
}

/* ---- clears ---- */
static id<MTLRenderPipelineState> clear_pso(MtlCtx *c, uint32_t flags) {
    uint32_t mask = 0;
    for (int i = 0; i < 4; i++) if (c->_rt[i].tex) mask |= 1u << i;
    int color = (flags & 1) && mask;
    NSString *key = [NSString stringWithFormat:@"clear:%@:%d", mtl_target_key(c), color];
    id<MTLRenderPipelineState> p = c.psos[key];
    if (p) return p;
    NSError *err = nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [c.util newFunctionWithName:@"fs_tri"];
    mtl_pipeline_targets(c, pd);
    if (mask) {
        NSMutableString *src = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                                                 "struct COut {"];
        for (int i = 0; i < 4; i++) if (mask & (1u << i)) [src appendFormat:@" float4 c%d [[color(%d)]];", i, i];
        [src appendString:@" };\nfragment COut clear_frag(constant float4 &c [[buffer(0)]]) { COut o;"];
        for (int i = 0; i < 4; i++) if (mask & (1u << i)) [src appendFormat:@" o.c%d = c;", i];
        [src appendString:@" return o; }\n"];
        id<MTLLibrary> lib = [c.dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) { fprintf(stderr, "[bw2] FATAL: clear shader: %s\n", err.description.UTF8String); abort(); }
        pd.fragmentFunction = [lib newFunctionWithName:@"clear_frag"];
        for (int i = 0; i < 4; i++)
            if (mask & (1u << i)) pd.colorAttachments[i].writeMask = color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    }
    p = [c.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) { fprintf(stderr, "[bw2] FATAL: clear pipeline: %s\n", err.description.UTF8String); abort(); }
    c.psos[key] = p;
    return p;
}

static id<MTLDepthStencilState> clear_dss(MtlCtx *c, int z, int stencil) {
    NSString *key = [NSString stringWithFormat:@"clear:%d%d", z, stencil];
    id<MTLDepthStencilState> s = c.dss[key];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionAlways;
    d.depthWriteEnabled = z ? YES : NO;
    if (stencil) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.readMask = sd.writeMask = 0xFF;
        d.frontFaceStencil = d.backFaceStencil = sd;
    }
    s = [c.dev newDepthStencilStateWithDescriptor:d];
    c.dss[key] = s;
    return s;
}

void mtl_clear(struct mtl_ctx *cc, uint32_t flags, uint32_t color, float z, uint32_t stencil,
               const struct mtl_rect *rects, uint32_t n) {
    MtlCtx *c = ctx(cc);
    int has_ds = c->_ds.tex != NULL;
    int dsf = has_ds && (flags & 2), stf = has_ds && (flags & 4) &&
              mtl_has_stencil(((__bridge id<MTLTexture>)c->_ds.tex).pixelFormat);
    if (!(flags & 1) && !dsf && !stf) return;
    mtl_begin_pass_if_needed(cc);
    id<MTLRenderCommandEncoder> e = c.enc;
    [e setRenderPipelineState:clear_pso(c, flags)];
    [e setDepthStencilState:clear_dss(c, dsf, stf)];
    [e setCullMode:MTLCullModeNone];
    [e setTriangleFillMode:MTLTriangleFillModeFill];
    [e setDepthBias:0 slopeScale:0 clamp:0];
    [e setStencilReferenceValue:stencil & 0xFF];
    [e setViewport:(MTLViewport){ 0, 0, c.passW, c.passH, 0, 1 }];
    float zz = z < 0 ? 0 : z > 1 ? 1 : z;
    float col[4] = { ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
                     ((color >> 24) & 0xFF) / 255.0f };
    [e setVertexBytes:&zz length:4 atIndex:0];
    [e setFragmentBytes:col length:16 atIndex:0];
    if (c.occl) [e setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = MIN(rects[i].x, c.passW), y = MIN(rects[i].y, c.passH);
        uint32_t w = MIN(rects[i].w, c.passW - x), h = MIN(rects[i].h, c.passH - y);
        if (!w || !h) continue;
        [e setScissorRect:(MTLScissorRect){ x, y, w, h }];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    if (c.occl) occl_count(c);                    /* counting goes on, in a fresh slot */
    c.encGen++;                                   /* draws must set their state again */
}

/* ---- present ---- */
void mtl_present(struct mtl_ctx *cc, mtl_obj backbuffer) {
    MtlCtx *c = ctx(cc);
    id<MTLTexture> bb = (__bridge id<MTLTexture>)backbuffer;
    mtl_end_encoders(c);
    dispatch_semaphore_wait(c.inflight, DISPATCH_TIME_FOREVER);
    id<CAMetalDrawable> drawable = nil;
    id<MTLTexture> target = nil;
    if (c.layer) {
        drawable = [c.layer nextDrawable];
        target = drawable.texture;
    } else target = g_test_drawable;
    id<MTLCommandBuffer> cb = cmdbuf(c);
    if (target) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        double tw = target.width, th = target.height, bw = bb.width, bh = bb.height;
        double s = MIN(tw / bw, th / bh), vw = floor(bw * s + 0.5), vh = floor(bh * s + 0.5);
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:c.presentPso];
        [e setViewport:(MTLViewport){ floor((tw - vw) / 2), floor((th - vh) / 2), vw, vh, 0, 1 }];
        float z = 0;
        [e setVertexBytes:&z length:4 atIndex:0];
        [e setFragmentTexture:bb atIndex:0];
        [e setFragmentSamplerState:c.linear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        if (drawable) [cb presentDrawable:drawable];
    }
    dispatch_semaphore_t sem = c.inflight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(sem); }];
    mtl_commit(c, !drawable && target);                       /* the test drawable is read right after */
    c.frames++;
}
uint64_t mtl_frame(struct mtl_ctx *cc) { return ctx(cc).frames; }

/* ---- StretchRect ---- */
void mtl_stretch(struct mtl_ctx *cc, mtl_obj src, const char *swizzle, uint32_t sslice, uint32_t slevel,
                 const struct mtl_rect *sr, struct mtl_target dst, const struct mtl_rect *dr, int linear) {
    MtlCtx *c = ctx(cc);
    id<MTLTexture> s = (__bridge id<MTLTexture>)src, d = (__bridge id<MTLTexture>)dst.tex;
    MTLTextureSwizzle map[4] = { MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                 MTLTextureSwizzleAlpha };
    for (int i = 0; swizzle && swizzle[0] && i < 4; i++)
        map[i] = swizzle[i] == 'r' ? MTLTextureSwizzleRed : swizzle[i] == 'g' ? MTLTextureSwizzleGreen
               : swizzle[i] == 'b' ? MTLTextureSwizzleBlue : swizzle[i] == 'a' ? MTLTextureSwizzleAlpha
               : swizzle[i] == '0' ? MTLTextureSwizzleZero : MTLTextureSwizzleOne;
    id<MTLTexture> v = [s newTextureViewWithPixelFormat:s.pixelFormat textureType:MTLTextureType2D
                                                 levels:NSMakeRange(slevel, 1) slices:NSMakeRange(sslice, 1)
                                                swizzle:MTLTextureSwizzleChannelsMake(map[0], map[1], map[2], map[3])];
    NSString *key = [NSString stringWithFormat:@"copy:%lu", (unsigned long)d.pixelFormat];
    id<MTLRenderPipelineState> p = c.psos[key];
    if (!p) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [c.util newFunctionWithName:@"copy_vert"];
        pd.fragmentFunction = [c.util newFunctionWithName:@"copy_frag"];
        pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        NSError *err = nil;
        p = [c.dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p) { fprintf(stderr, "[bw2] FATAL: copy pipeline: %s\n", err.description.UTF8String); abort(); }
        c.psos[key] = p;
    }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> smp = [c.dev newSamplerStateWithDescriptor:sd];
    mtl_end_encoders(c);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d;
    rp.colorAttachments[0].slice = dst.slice, rp.colorAttachments[0].level = dst.level;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmdbuf(c) renderCommandEncoderWithDescriptor:rp];
    double sw = v.width, sh = v.height;
    struct { float r[4]; float lod; float pad[3]; } u = {
        { (float)(sr->x / sw), (float)(sr->y / sh), (float)(sr->w / sw), (float)(sr->h / sh) }, 0, { 0 } };
    [e setRenderPipelineState:p];
    [e setViewport:(MTLViewport){ dr->x, dr->y, dr->w, dr->h, 0, 1 }];
    [e setVertexBytes:&u length:sizeof u atIndex:0];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e setFragmentTexture:v atIndex:0];
    [e setFragmentSamplerState:smp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    c.encGen++;
}
