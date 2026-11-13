/*
 * d3dx_dxt.c - DXT1, DXT3 and DXT5 compression for D3DX (plan 10 Task 3, Ruling 9). Our own code:
 * a block's colour lies along the principal axis of its colours (power iteration on their
 * covariance), the endpoints are the extremes of the colours' projections rounded to 5:6:5, and one
 * least-squares refit of the endpoints from the chosen indices is kept when it lowers the error.
 * Indices are the nearest palette entry, the palette built as d3dx_pixel.c decodes it.
 * DXT1 uses its 3-colour mode (index 3 transparent) when any texel has alpha < 0.5, and only the
 * opaque texels shape the colour. DXT3 alpha is round(a * 15). DXT5 alpha uses the 8-value mode
 * between the block's minimum and maximum alpha. Equal endpoints give all-zero indices.
 * DXT2 and DXT4 (premultiplied alpha) stop loudly: the game was not seen to use them.
 */
#include <math.h>
#include <string.h>
#include "d3d_com.h"
#include "d3d9_format.h"
#include "d3dx.h"

static float clamp01(float v) { return fminf(fmaxf(v, 0.0f), 1.0f); }
static float x5(uint32_t v) { return (float)((v << 3) | (v >> 2)) / 255.0f; }
static float x6(uint32_t v) { return (float)((v << 2) | (v >> 4)) / 255.0f; }

/* The channel value whose widening (bit replication, as the decoder does it) is nearest v. */
static uint32_t q_chan(float v, uint32_t bits) {
    int m = (1 << bits) - 1, q = (int)lrintf(v * (float)m), best = q;
    float e = INFINITY;
    for (int k = q - 1; k <= q + 1; k++) {
        if (k < 0 || k > m) continue;
        float d = fabsf((bits == 5 ? x5((uint32_t)k) : x6((uint32_t)k)) - v);
        if (d < e) e = d, best = k;
    }
    return (uint32_t)best;
}
static uint16_t q565(const float *c) {
    return (uint16_t)(q_chan(c[0], 5) << 11 | q_chan(c[1], 6) << 5 | q_chan(c[2], 5));
}
static void e565(uint16_t v, float *o) { o[0] = x5(v >> 11), o[1] = x6((v >> 5) & 63), o[2] = x5(v & 31); }

/* Indices for the texels in use against endpoints c0, c1 (three: the 3-colour palette); the squared error. */
static float pick(const float *px, const int *use, uint16_t c0, uint16_t c1, int three, uint8_t *idx) {
    float pal[4][3];
    e565(c0, pal[0]), e565(c1, pal[1]);
    for (int k = 0; k < 3; k++)
        if (three) pal[2][k] = (pal[0][k] + pal[1][k]) / 2, pal[3][k] = 0;
        else pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3, pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    float err = 0;
    for (int i = 0; i < 16; i++) {
        if (!use[i]) continue;
        float best = INFINITY;
        for (int k = 0; k < (three ? 3 : 4); k++) {
            float d = 0;
            for (int c = 0; c < 3; c++) d += (px[4 * i + c] - pal[k][c]) * (px[4 * i + c] - pal[k][c]);
            if (d < best) best = d, idx[i] = (uint8_t)k;
        }
        err += best;
    }
    return err;
}

/* The colour half of a block: c0, c1 and 16 two-bit indices into out[0..7]. */
static void encode_color(const float *px, const int *use, int three, uint8_t *out) {
    static const float w4[4] = { 1, 0, 2.0f / 3, 1.0f / 3 }, w3[3] = { 1, 0, 0.5f };
    const float *wt = three ? w3 : w4;
    uint8_t idx[16] = { 0 };
    uint16_t c0 = 0, c1 = 0;
    float mean[3] = { 0 };
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (use[i]) { for (int c = 0; c < 3; c++) mean[c] += px[4 * i + c]; n++; }
    if (n) {
        for (int c = 0; c < 3; c++) mean[c] /= (float)n;
        float cov[3][3] = { { 0 } };
        for (int i = 0; i < 16; i++)
            if (use[i])
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++) cov[a][b] += (px[4 * i + a] - mean[a]) * (px[4 * i + b] - mean[b]);
        /* power iteration from the covariance's largest column (never orthogonal to the axis) */
        int col = 0;
        float cn[3];
        for (int b = 0; b < 3; b++) cn[b] = cov[0][b] * cov[0][b] + cov[1][b] * cov[1][b] + cov[2][b] * cov[2][b];
        if (cn[1] > cn[col]) col = 1;
        if (cn[2] > cn[col]) col = 2;
        float axis[3] = { cov[0][col], cov[1][col], cov[2][col] };
        for (int it = 0; it < 8; it++) {
            float len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            if (len < 1e-20f) { axis[0] = axis[1] = axis[2] = 0; break; }
            float v[3] = { axis[0] / len, axis[1] / len, axis[2] / len };
            for (int a = 0; a < 3; a++) axis[a] = cov[a][0] * v[0] + cov[a][1] * v[1] + cov[a][2] * v[2];
            if (it == 7) memcpy(axis, v, sizeof v);
        }
        float tmin = INFINITY, tmax = -INFINITY;
        for (int i = 0; i < 16; i++) {
            if (!use[i]) continue;
            float t = 0;
            for (int c = 0; c < 3; c++) t += (px[4 * i + c] - mean[c]) * axis[c];
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
        }
        float e0[3], e1[3];
        for (int c = 0; c < 3; c++) e0[c] = clamp01(mean[c] + tmax * axis[c]), e1[c] = clamp01(mean[c] + tmin * axis[c]);
        c0 = q565(e0), c1 = q565(e1);
        if (c0 != c1) {
            float err = pick(px, use, c0, c1, three, idx);
            /* the least-squares endpoints for these indices, per channel */
            float aa = 0, ab = 0, bb = 0, xa[3] = { 0 }, xb[3] = { 0 };
            for (int i = 0; i < 16; i++) {
                if (!use[i]) continue;
                float a = wt[idx[i]], b = 1 - a;
                aa += a * a, ab += a * b, bb += b * b;
                for (int c = 0; c < 3; c++) xa[c] += a * px[4 * i + c], xb[c] += b * px[4 * i + c];
            }
            float det = aa * bb - ab * ab;
            if (fabsf(det) > 1e-6f) {
                float r0[3], r1[3];
                for (int c = 0; c < 3; c++)
                    r0[c] = clamp01((bb * xa[c] - ab * xb[c]) / det), r1[c] = clamp01((aa * xb[c] - ab * xa[c]) / det);
                uint16_t d0 = q565(r0), d1 = q565(r1);
                uint8_t idx2[16] = { 0 };
                if (d0 != d1 && pick(px, use, d0, d1, three, idx2) < err) c0 = d0, c1 = d1, memcpy(idx, idx2, sizeof idx);
            }
        }
    }
    if (c0 == c1) memset(idx, 0, sizeof idx);
    else if (!three && c0 < c1) {                            /* 4 colours: c0 > c1 */
        uint16_t t = c0; c0 = c1, c1 = t;
        for (int i = 0; i < 16; i++) idx[i] ^= 1;
    } else if (three && c0 > c1) {                           /* 3 colours: c0 <= c1 */
        uint16_t t = c0; c0 = c1, c1 = t;
        for (int i = 0; i < 16; i++) if (idx[i] < 2) idx[i] ^= 1;
    }
    uint32_t bits = 0;
    for (int i = 0; i < 16; i++) bits |= (uint32_t)(use[i] ? idx[i] : 3) << (2 * i);
    out[0] = (uint8_t)c0, out[1] = (uint8_t)(c0 >> 8), out[2] = (uint8_t)c1, out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)bits, out[5] = (uint8_t)(bits >> 8), out[6] = (uint8_t)(bits >> 16), out[7] = (uint8_t)(bits >> 24);
}

/* DXT5's alpha half: the 8-value mode between the block's extremes, nearest index. */
static void encode_alpha8(const float *px, uint8_t *out) {
    float lo = 1, hi = 0;
    for (int i = 0; i < 16; i++) lo = fminf(lo, px[4 * i + 3]), hi = fmaxf(hi, px[4 * i + 3]);
    uint32_t a0 = (uint32_t)lrintf(hi * 255), a1 = (uint32_t)lrintf(lo * 255);
    uint64_t bits = 0;
    if (a0 > a1) {
        float pal[8], f0 = (float)a0 / 255.0f, f1 = (float)a1 / 255.0f;   /* as d3dx_pixel.c decodes it */
        pal[0] = f0, pal[1] = f1;
        for (int k = 1; k < 7; k++) pal[k + 1] = ((float)(7 - k) * f0 + (float)k * f1) / 7;
        for (int i = 0; i < 16; i++) {
            int best = 0;
            for (int k = 1; k < 8; k++)
                if (fabsf(px[4 * i + 3] - pal[k]) < fabsf(px[4 * i + 3] - pal[best])) best = k;
            bits |= (uint64_t)best << (3 * i);
        }
    } else a1 = a0;                                          /* equal endpoints, all-zero indices */
    out[0] = (uint8_t)a0, out[1] = (uint8_t)a1;
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

void d3dx_dxt_encode(uint32_t fmt, const float rgba[64], uint8_t *block) {
    if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT4)
        d3d_fail("D3DX: compressing into %s (premultiplied alpha) is not supported", fmt == D3DFMT_DXT2 ? "DXT2" : "DXT4");
    if (fmt != D3DFMT_DXT1 && fmt != D3DFMT_DXT3 && fmt != D3DFMT_DXT5)
        d3d_fail("D3DX: the DXT encoder was asked for format 0x%08X", fmt);
    float px[64];
    for (int i = 0; i < 64; i++) px[i] = clamp01(rgba[i]);
    int use[16], three = 0;
    for (int i = 0; i < 16; i++) {
        use[i] = fmt != D3DFMT_DXT1 || px[4 * i + 3] >= 0.5f;
        if (!use[i]) three = 1;
    }
    if (fmt == D3DFMT_DXT1) { encode_color(px, use, three, block); return; }
    if (fmt == D3DFMT_DXT3) {
        memset(block, 0, 8);
        for (int i = 0; i < 16; i++) block[i / 2] |= (uint8_t)(lrintf(px[4 * i + 3] * 15) << (4 * (i & 1)));
    } else encode_alpha8(px, block);
    encode_color(px, use, 0, block + 8);
}
