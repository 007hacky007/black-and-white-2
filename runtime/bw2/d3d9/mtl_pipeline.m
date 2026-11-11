/*
 * mtl_pipeline.m - draws on Metal (plan 4 Task 6, plan 5): libraries
 * compiled from ff_msl.c's source and cached by it, translated shaders'
 * libraries compiled on a background queue, render pipelines cached by
 * their whole key and kept in a binary archive on disk, depth-stencil and
 * sampler states cached by value, and the draw itself.
 */
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "ff.h"
#include "mtl_internal.h"

static NSMutableDictionary<NSString *, id<MTLLibrary>> *g_libs;
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *g_psos;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *g_dss;
static NSMutableDictionary<NSData *, id<MTLSamplerState>> *g_smps;
static struct mtl_stats g_stats;
/* The pipeline cache on disk: a directory of archives. Metal cannot add to an archive it loaded and
 * save it again, so each session loads every archive there read-only and saves what it added as
 * an archive of its own; past MAX_ARCHIVES files the cache starts again, cold. */
#define MAX_ARCHIVES 8
static NSMutableArray<id<MTLBinaryArchive>> *g_loaded;
static id<MTLBinaryArchive> g_archive;             /* this session's additions */
static NSURL *g_archive_dir;
static int g_require_hits, g_archive_dirty;

void mtl_get_stats(struct mtl_stats *s) { *s = g_stats; }
void mtl_archive_require_hits(int on) { g_require_hits = on; }

static id<MTLDevice> dev0(void) { return MTLCreateSystemDefaultDevice(); }

/* ---- translated shaders: one library per distinct source, compiled in the background ---- */
@interface MtlAsyncLib : NSObject
@property(strong) id<MTLLibrary> lib;
@property(strong) NSError *err;
@property(strong) NSString *src, *name;
@property(strong) dispatch_group_t done;
@end
@implementation MtlAsyncLib
@end
static NSMutableDictionary<NSString *, MtlAsyncLib *> *g_async;

/* D3D9's results need IEEE infinities (rcp(0), log(0)): no fast math, which may assume there are none */
static MTLCompileOptions *sm_options(void) {
    MTLCompileOptions *o = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeRelaxed;
    else o.fastMathEnabled = NO;
    return o;
}

mtl_obj mtl_library_async(const char *msl, const char *name) {
    if (!g_async) g_async = [NSMutableDictionary dictionary];
    NSString *s = [NSString stringWithUTF8String:msl];
    MtlAsyncLib *l = g_async[s];
    if (!l) {
        l = [MtlAsyncLib new];
        l.src = s, l.name = [NSString stringWithUTF8String:name];
        l.done = dispatch_group_create();
        dispatch_group_enter(l.done);
        g_stats.libraries++;
        MtlAsyncLib *w = l;
        [dev0() newLibraryWithSource:s options:sm_options()
                   completionHandler:^(id<MTLLibrary> lib, NSError *err) {
                       w.lib = lib, w.err = err;
                       dispatch_group_leave(w.done);
                   }];
        g_async[s] = l;
    }
    return (__bridge_retained void *)l;
}

static id<MTLLibrary> library_wait(mtl_obj h) {
    MtlAsyncLib *l = (__bridge MtlAsyncLib *)h;
    dispatch_group_wait(l.done, DISPATCH_TIME_FOREVER);
    if (!l.lib) {
        fprintf(stderr, "[bw2] FATAL: d3d9: %s does not compile: %s\n%s\n", l.name.UTF8String,
                l.err.description.UTF8String, l.src.UTF8String);
        abort();
    }
    return l.lib;
}

void mtl_archive_save(void) {
    if (!g_archive || !g_archive_dir || !g_archive_dirty) return;
    NSError *err = nil;
    [[NSFileManager defaultManager] createDirectoryAtURL:g_archive_dir withIntermediateDirectories:YES
                                              attributes:nil error:nil];
    NSString *name = [NSString stringWithFormat:@"ff-%@.metalar", [[NSUUID UUID] UUIDString]];
    NSURL *url = [g_archive_dir URLByAppendingPathComponent:name];
    if (![g_archive serializeToURL:url error:&err])
        fprintf(stderr, "[bw2] metal: pipeline archive not saved: %s\n", err.description.UTF8String);
    g_archive_dirty = 0;
    /* what was saved is now one of the loaded archives; later additions start a new one */
    MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
    d.url = url;
    id<MTLBinaryArchive> saved = [dev0() newBinaryArchiveWithDescriptor:d error:nil];
    if (saved) [g_loaded addObject:saved];
    g_archive = [dev0() newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
}
static void save_at_exit(void) { mtl_archive_save(); }

void mtl_archive_open(const char *dir) {
    static int hooked;
    if (!hooked) { atexit(save_at_exit); hooked = 1; }
    mtl_archive_save();
    g_archive_dir = [NSURL fileURLWithPath:[NSString stringWithUTF8String:dir] isDirectory:YES];
    g_loaded = [NSMutableArray array];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSURL *> *files = [fm contentsOfDirectoryAtURL:g_archive_dir includingPropertiesForKeys:nil options:0
                                                     error:nil];
    NSMutableArray<NSURL *> *archives = [NSMutableArray array];
    for (NSURL *u in files) if ([u.pathExtension isEqualToString:@"metalar"]) [archives addObject:u];
    if (archives.count > MAX_ARCHIVES) {
        for (NSURL *u in archives) [fm removeItemAtURL:u error:nil];
        [archives removeAllObjects];
    }
    for (NSURL *u in archives) {
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
        d.url = u;
        id<MTLBinaryArchive> a = [dev0() newBinaryArchiveWithDescriptor:d error:nil];
        if (a) [g_loaded addObject:a];
        else [fm removeItemAtURL:u error:nil];             /* unreadable (another OS, another GPU) */
    }
    g_archive = [dev0() newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
    g_archive_dirty = 0;
}

/* A fixed-function library from its source (src is freed). */
static id<MTLLibrary> library(id<MTLDevice> dev, char *src) {
    if (!g_libs) g_libs = [NSMutableDictionary dictionary];
    NSString *s = [NSString stringWithUTF8String:src];
    free(src);
    id<MTLLibrary> lib = g_libs[s];
    if (lib) return lib;
    NSError *err = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.fastMathEnabled = YES;
    lib = [dev newLibraryWithSource:s options:o error:&err];
    if (!lib) {
        fprintf(stderr, "[bw2] FATAL: d3d9: fixed-function shader does not compile: %s\n%s\n",
                err.description.UTF8String, s.UTF8String);
        abort();
    }
    g_stats.libraries++;
    g_libs[s] = lib;
    return lib;
}

static MTLVertexFormat vformat(uint32_t t) {
    switch (t) {
    case 0: return MTLVertexFormatFloat;
    case 1: return MTLVertexFormatFloat2;
    case 2: return MTLVertexFormatFloat3;
    case 3: return MTLVertexFormatFloat4;
    case 4: return MTLVertexFormatUChar4Normalized_BGRA;
    case 5: return MTLVertexFormatUChar4;
    case 6: return MTLVertexFormatShort2;
    case 7: return MTLVertexFormatShort4;
    case 8: return MTLVertexFormatUChar4Normalized;
    case 9: return MTLVertexFormatShort2Normalized;
    case 10: return MTLVertexFormatShort4Normalized;
    case 11: return MTLVertexFormatUShort2Normalized;
    case 12: return MTLVertexFormatUShort4Normalized;
    case 14: return MTLVertexFormatInt1010102Normalized;
    case 15: return MTLVertexFormatHalf2;
    case 16: return MTLVertexFormatHalf4;
    default:
        fprintf(stderr, "[bw2] FATAL: d3d9: vertex element type %u is not supported\n", t);
        abort();
    }
}
static MTLBlendFactor bfactor(uint32_t f, int alpha) {
    switch (f) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
    case 4: return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
    case 10: return alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 15: return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation bop(uint32_t o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
static MTLCompareFunction cmpf(uint32_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
static MTLStencilOperation sop(uint32_t o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}
static MTLColorWriteMask wmask(uint32_t m) {
    return (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
           (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
}

/* Blend factors D3D names that are really two (BOTHSRCALPHA, BOTHINVSRCALPHA). */
static void both(uint32_t *src, uint32_t *dst) {
    if (*src == 12) *src = 5, *dst = 6;
    else if (*src == 13) *src = 6, *dst = 5;
}

static id<MTLRenderPipelineState> pipeline(MtlCtx *c, const struct ff_draw *d) {
    if (!g_psos) g_psos = [NSMutableDictionary dictionary];
    struct {
        struct ff_vkey vk; struct ff_pkey pk;
        uint64_t vs_hash, ps_hash;
        uint8_t vs_var[32], ps_var[32];
        uint32_t nattr; struct ff_attr attr[VA_COUNT]; uint32_t stride[16];
        uint32_t blend, src, dst, op, srca, dsta, opa, wmask[4];
        uint64_t fmt[5];
        uint32_t points;
    } key;
    memset(&key, 0, sizeof key);
    if (!d->vs) key.vk = *d->vk;          /* with a shader, the stage's key is the shader and its variant */
    else key.vs_hash = d->vs->hash, memcpy(key.vs_var, d->vs->variant, sizeof key.vs_var);
    key.pk = *d->pk, key.nattr = d->nattr;
    key.points = d->prim == 1;
    memcpy(key.attr, d->attr, sizeof d->attr[0] * d->nattr);
    for (uint32_t i = 0; i < d->nattr; i++) key.stride[d->attr[i].stream] = d->stream_stride[d->attr[i].stream];
    key.blend = d->fx.blend, key.src = d->fx.src, key.dst = d->fx.dst, key.op = d->fx.op;
    key.srca = d->fx.srca, key.dsta = d->fx.dsta, key.opa = d->fx.opa;
    both(&key.src, &key.dst);
    both(&key.srca, &key.dsta);
    if (!key.blend) key.src = key.dst = key.op = key.srca = key.dsta = key.opa = 0;
    for (int i = 0; i < 4; i++) key.wmask[i] = d->fx.wmask[i];
    for (int i = 0; i < 4; i++) key.fmt[i] = c->_rt[i].tex ? ((__bridge id<MTLTexture>)c->_rt[i].tex).pixelFormat : 0;
    key.fmt[4] = c->_ds.tex ? ((__bridge id<MTLTexture>)c->_ds.tex).pixelFormat : 0;
    NSData *k = [NSData dataWithBytes:&key length:sizeof key];
    id<MTLRenderPipelineState> p = g_psos[k];
    if (p) return p;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    if (d->vs) {                                        /* the stages from separate libraries */
        pd.vertexFunction = [library_wait(d->vs->lib) newFunctionWithName:@"vsh"];
        pd.fragmentFunction = [library(c.dev, ff_msl_fragment(d->pk, 8)) newFunctionWithName:@"fff"];
    } else {
        id<MTLLibrary> lib = library(c.dev, ff_msl(d->vk, d->pk));
        pd.vertexFunction = [lib newFunctionWithName:@"ffv"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fff"];
    }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < d->nattr; i++) {
        const struct ff_attr *a = &d->attr[i];
        vd.attributes[a->slot].format = vformat(a->decltype);
        vd.attributes[a->slot].offset = a->offset;
        vd.attributes[a->slot].bufferIndex = FF_STREAM_BUFFER0 + a->stream;
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[FF_STREAM_BUFFER0 + a->stream];
        uint32_t stride = d->stream_stride[a->stream];
        if (stride) l.stride = stride, l.stepFunction = MTLVertexStepFunctionPerVertex;
        else l.stride = 16, l.stepFunction = MTLVertexStepFunctionConstant, l.stepRate = 0;
    }
    pd.vertexDescriptor = vd;
    mtl_pipeline_targets(c, pd);
    for (int i = 0; i < 4; i++) {
        if (!c->_rt[i].tex) continue;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        ca.writeMask = wmask(key.wmask[i]);
        if (key.blend) {
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = bfactor(key.src, 0), ca.destinationRGBBlendFactor = bfactor(key.dst, 0);
            ca.rgbBlendOperation = bop(key.op);
            ca.sourceAlphaBlendFactor = bfactor(key.srca, 1), ca.destinationAlphaBlendFactor = bfactor(key.dsta, 1);
            ca.alphaBlendOperation = bop(key.opa);
        }
    }
    if (key.points) pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
    NSError *err = nil;
    if (g_archive) {
        NSMutableArray *all = [NSMutableArray arrayWithArray:g_loaded];
        [all addObject:g_archive];
        pd.binaryArchives = all;
    }
    if (g_archive && g_require_hits) {
        p = [c.dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                             reflection:nil error:&err];
        if (p) g_stats.archive_hits++;
        else g_stats.archive_misses++;
    }
    if (!p) p = [c.dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) {
        fprintf(stderr, "[bw2] FATAL: d3d9: %s pipeline: %s\n", d->vs ? "vertex shader" : "fixed-function",
                err.description.UTF8String);
        abort();
    }
    if (g_archive && !g_require_hits) {
        pd.binaryArchives = nil;                        /* the session's archive gets the functions themselves */
        if ([g_archive addRenderPipelineFunctionsWithDescriptor:pd error:&err]) g_archive_dirty = 1;
    }
    g_stats.pipelines++;
    g_psos[k] = p;
    return p;
}

static id<MTLDepthStencilState> depth_stencil(MtlCtx *c, const struct ff_fixed *fx) {
    if (!g_dss) g_dss = [NSMutableDictionary dictionary];
    int has_ds = c->_ds.tex != NULL, has_st = has_ds && mtl_has_stencil(((__bridge id<MTLTexture>)c->_ds.tex).pixelFormat);
    uint32_t key[16] = { has_ds && fx->zenable, has_ds && fx->zenable && fx->zwrite, fx->zfunc, has_st && fx->stencil,
                         fx->twosided, fx->sfail, fx->szfail, fx->spass, fx->sfunc, fx->ccw_fail, fx->ccw_zfail,
                         fx->ccw_pass, fx->ccw_func, fx->smask & 0xFF, fx->swmask & 0xFF, 0 };
    if (!key[3]) memset(key + 4, 0, sizeof(uint32_t) * 11);
    if (!key[0]) key[1] = key[2] = 0;
    NSData *k = [NSData dataWithBytes:key length:sizeof key];
    id<MTLDepthStencilState> s = g_dss[k];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = key[0] ? cmpf(key[2]) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = key[1] ? YES : NO;
    if (key[3]) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        f.stencilFailureOperation = sop(key[5]), f.depthFailureOperation = sop(key[6]);
        f.depthStencilPassOperation = sop(key[7]), f.stencilCompareFunction = cmpf(key[8]);
        f.readMask = key[13], f.writeMask = key[14];
        MTLStencilDescriptor *b = f;
        if (key[4]) {
            b = [MTLStencilDescriptor new];
            b.stencilFailureOperation = sop(key[9]), b.depthFailureOperation = sop(key[10]);
            b.depthStencilPassOperation = sop(key[11]), b.stencilCompareFunction = cmpf(key[12]);
            b.readMask = key[13], b.writeMask = key[14];
        }
        d.frontFaceStencil = f, d.backFaceStencil = b;
    }
    s = [c.dev newDepthStencilStateWithDescriptor:d];
    g_dss[k] = s;
    return s;
}

static MTLSamplerAddressMode amode(uint32_t a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}
static id<MTLSamplerState> sampler(MtlCtx *c, const struct ff_sampler *s) {
    if (!g_smps) g_smps = [NSMutableDictionary dictionary];
    NSData *k = [NSData dataWithBytes:s length:sizeof *s];
    id<MTLSamplerState> r = g_smps[k];
    if (r) return r;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.sAddressMode = amode(s->u), d.tAddressMode = amode(s->v), d.rAddressMode = amode(s->w);
    int aniso = s->min == 3 || s->mag == 3;
    d.minFilter = s->min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = s->mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = s->mip == 0 ? MTLSamplerMipFilterNotMipmapped : s->mip == 1 ? MTLSamplerMipFilterNearest
                                                                              : MTLSamplerMipFilterLinear;
    if (aniso) d.maxAnisotropy = s->aniso < 1 ? 1 : s->aniso > 16 ? 16 : s->aniso;
    d.lodMinClamp = (float)s->maxmip;
    uint32_t b = s->border;
    uint32_t a = b >> 24, lum = ((b >> 16 & 0xFF) + (b >> 8 & 0xFF) + (b & 0xFF)) / 3;
    d.borderColor = a < 128 ? MTLSamplerBorderColorTransparentBlack
                  : lum < 128 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    if (s->u == 4 || s->v == 4 || s->w == 4) {
        static int warned;
        if (b != 0 && b != 0xFF000000u && b != 0xFFFFFFFFu && !warned++)
            fprintf(stderr, "[bw2] d3d9: border colour 0x%08X drawn as the nearest Metal one\n", b);
    }
    r = [c.dev newSamplerStateWithDescriptor:d];
    g_smps[k] = r;
    return r;
}

static MTLPrimitiveType ptype(uint32_t p) {
    switch (p) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

void mtl_draw(struct mtl_ctx *cc, const struct ff_draw *d) {
    MtlCtx *c = (__bridge MtlCtx *)(void *)cc;
    mtl_begin_pass_if_needed(cc);
    id<MTLRenderCommandEncoder> e = c.enc;
    [e setRenderPipelineState:pipeline(c, d)];
    [e setDepthStencilState:depth_stencil(c, &d->fx)];
    [e setStencilReferenceValue:d->fx.sref & 0xFF];
    /* D3D's CULL_CCW (the default) culls triangles that are counter-clockwise on screen */
    [e setFrontFacingWinding:MTLWindingClockwise];
    [e setCullMode:d->fx.cull == 2 ? MTLCullModeFront : d->fx.cull == 3 ? MTLCullModeBack : MTLCullModeNone];
    [e setTriangleFillMode:d->fx.fill == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    [e setDepthBias:d->fx.depth_bias * 16777216.0f slopeScale:d->fx.slope_bias clamp:0];
    uint32_t bf = d->fx.blend_factor;
    [e setBlendColorRed:((bf >> 16) & 0xFF) / 255.0f green:((bf >> 8) & 0xFF) / 255.0f blue:(bf & 0xFF) / 255.0f
                  alpha:(bf >> 24) / 255.0f];
    MTLViewport vp = { d->fx.vp[0], d->fx.vp[1], d->fx.vp[2], d->fx.vp[3], d->fx.vp[4], d->fx.vp[5] };
    [e setViewport:vp];
    uint32_t sx = 0, sy = 0, sw = c.passW, sh = c.passH;
    if (d->fx.scissor) {
        sx = MIN(d->fx.sc_x, c.passW), sy = MIN(d->fx.sc_y, c.passH);
        sw = MIN(d->fx.sc_w, c.passW - sx), sh = MIN(d->fx.sc_h, c.passH - sy);
        if (!sw || !sh) return;                                /* nothing survives the scissor test */
    }
    [e setScissorRect:(MTLScissorRect){ sx, sy, sw, sh }];
    [e setVertexBytes:d->vu length:sizeof *d->vu atIndex:0];
    [e setFragmentBytes:d->pu length:sizeof *d->pu atIndex:0];
    if (d->vs) {
        [e setVertexBytes:d->vs->f length:d->vs->fsize atIndex:1];
        [e setVertexBytes:d->vs->ib length:d->vs->ibsize atIndex:2];
    }
    for (int i = 0; i < 16; i++)
        if (d->stream_buf[i])
            [e setVertexBuffer:(__bridge id<MTLBuffer>)d->stream_buf[i] offset:d->stream_off[i]
                       atIndex:FF_STREAM_BUFFER0 + i];
    for (int i = 0; i < d->pk->nstages; i++)
        if (d->pk->textype[i]) {
            [e setFragmentTexture:(__bridge id<MTLTexture>)d->tex[i] atIndex:i];
            [e setFragmentSamplerState:sampler(c, &d->smp[i]) atIndex:i];
        }
    if (d->indexed)
        [e drawIndexedPrimitives:ptype(d->prim) indexCount:d->count
                       indexType:d->index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                     indexBuffer:(__bridge id<MTLBuffer>)d->ibuf indexBufferOffset:d->ioff instanceCount:1
                      baseVertex:d->base_vertex baseInstance:0];
    else [e drawPrimitives:ptype(d->prim) vertexStart:d->start vertexCount:d->count];
}

void mtl_pipeline_cache_drop(void) {
    [g_libs removeAllObjects];
    [g_async removeAllObjects];
    [g_psos removeAllObjects];
}
