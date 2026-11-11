/*
 * d3dx_imageio.m - PNG and JPEG for D3DX through ImageIO (plan 4 Task 9).
 * Decoded straight from the image's own pixels when they are 8-bit RGB(A),
 * so alpha is not premultiplied; anything else is drawn into a bitmap.
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <stdlib.h>
#include <string.h>
#include "d3dx.h"

int d3dx_imageio_decode(const uint8_t *buf, uint32_t n, uint32_t *w, uint32_t *h, int *has_alpha, uint8_t **argb) {
    @autoreleasepool {
        CFDataRef data = CFDataCreate(NULL, buf, n);
        CGImageSourceRef src = CGImageSourceCreateWithData(data, NULL);
        CFRelease(data);
        if (!src) return 0;
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        CFRelease(src);
        if (!img) return 0;
        size_t iw = CGImageGetWidth(img), ih = CGImageGetHeight(img);
        CGImageAlphaInfo ai = CGImageGetAlphaInfo(img);
        CGBitmapInfo bi = CGImageGetBitmapInfo(img);
        size_t bpc = CGImageGetBitsPerComponent(img), bpp = CGImageGetBitsPerPixel(img), stride = CGImageGetBytesPerRow(img);
        int alpha = ai == kCGImageAlphaLast || ai == kCGImageAlphaFirst || ai == kCGImageAlphaPremultipliedLast ||
                    ai == kCGImageAlphaPremultipliedFirst;
        uint8_t *out = malloc(iw * ih * 4);
        int order_little = (bi & kCGBitmapByteOrderMask) == kCGBitmapByteOrder32Little;
        int direct = bpc == 8 && (bpp == 32 || bpp == 24) && !order_little &&
                     (ai == kCGImageAlphaLast || ai == kCGImageAlphaNone || ai == kCGImageAlphaNoneSkipLast);
        if (direct) {
            CFDataRef px = CGDataProviderCopyData(CGImageGetDataProvider(img));
            const uint8_t *p = CFDataGetBytePtr(px);
            size_t k = bpp / 8;
            for (size_t y = 0; y < ih; y++)
                for (size_t x = 0; x < iw; x++) {
                    const uint8_t *s = p + y * stride + x * k;
                    uint8_t *d = out + 4 * (y * iw + x);
                    d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = ai == kCGImageAlphaLast ? s[3] : 0xFF;
                }
            CFRelease(px);
        } else {                                         /* through a bitmap; alpha unpremultiplied after */
            uint8_t *rgba = calloc(iw * ih, 4);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef cg = CGBitmapContextCreate(rgba, iw, ih, 8, iw * 4, cs, kCGImageAlphaPremultipliedLast);
            CGColorSpaceRelease(cs);
            CGContextDrawImage(cg, CGRectMake(0, 0, iw, ih), img);
            CGContextRelease(cg);
            for (size_t i = 0; i < iw * ih; i++) {
                uint8_t *s = rgba + 4 * i, *d = out + 4 * i, a = s[3];
                for (int c = 0; c < 3; c++) {
                    unsigned v = a ? (s[c] * 255u + a / 2) / a : 0;
                    d[2 - c] = (uint8_t)(v > 255 ? 255 : v);
                }
                d[3] = alpha ? a : 0xFF;
            }
            free(rgba);
        }
        CGImageRelease(img);
        *w = (uint32_t)iw, *h = (uint32_t)ih, *has_alpha = alpha, *argb = out;
        return 1;
    }
}

uint8_t *d3dx_imageio_encode(const float *c, uint32_t w, uint32_t h, int jpeg, uint32_t *n) {
    @autoreleasepool {
        uint8_t *rgba = malloc((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h * 4; i++) {
            float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
            rgba[i] = (uint8_t)(f * 255.0f + 0.5f);
        }
        CFDataRef px = CFDataCreate(NULL, rgba, (CFIndex)w * h * 4);
        free(rgba);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(px);
        CFRelease(px);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, (CGBitmapInfo)kCGImageAlphaLast, prov, NULL, false,
                                       kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(prov);
        CFMutableDataRef out = CFDataCreateMutable(NULL, 0);
        CGImageDestinationRef dst = CGImageDestinationCreateWithData(out, (__bridge CFStringRef)(jpeg ? @"public.jpeg" : @"public.png"), 1, NULL);
        CGImageDestinationAddImage(dst, img, NULL);
        bool ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
        CGImageRelease(img);
        uint8_t *r = NULL;
        if (ok) {
            *n = (uint32_t)CFDataGetLength(out);
            r = malloc(*n);
            memcpy(r, CFDataGetBytePtr(out), *n);
        }
        CFRelease(out);
        return r;
    }
}
