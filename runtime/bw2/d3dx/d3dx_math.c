/*
 * d3dx_math.c - the 30 math functions BW2 imports from d3dx9_25.dll (plan 4
 * Task 8), from their documented formulas, in float. Every one is stdcall and
 * returns its output pointer (D3DXMatrixDeterminant returns a float in st(0));
 * every output may be one of the inputs.
 */
#include <math.h>
#include <string.h>
#include "win32hle.h"

static float F(int i) { float f; uint32_t u = A32(i); memcpy(&f, &u, 4); return f; }
static void get(float *dst, uint32_t va, int n) { memcpy(dst, hle_ptr(va), 4 * (size_t)n); }
static void put(uint32_t va, const float *src, int n) { memcpy(hle_ptr(va), src, 4 * (size_t)n); }

/* 4x4, row-major, row vectors: v' = v * M */
static void mmul(float *o, const float *a, const float *b) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}
static void ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

static float det3(float a, float b, float c, float d, float e, float f, float g, float h, float i) {
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}
static float determinant(const float *m) {
    return m[0] * det3(m[5], m[6], m[7], m[9], m[10], m[11], m[13], m[14], m[15]) -
           m[1] * det3(m[4], m[6], m[7], m[8], m[10], m[11], m[12], m[14], m[15]) +
           m[2] * det3(m[4], m[5], m[7], m[8], m[9], m[11], m[12], m[13], m[15]) -
           m[3] * det3(m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
}
/* The inverse by cofactors; 0 for a singular matrix. */
static int inverse(float *o, float *det_out, const float *m) {
    float c[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s[9];
            int k = 0;
            for (int r = 0; r < 4; r++)
                for (int q = 0; q < 4; q++)
                    if (r != i && q != j) s[k++] = m[r * 4 + q];
            float v = det3(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8]);
            c[j * 4 + i] = ((i + j) & 1) ? -v : v;           /* the adjugate: transposed cofactors */
        }
    float det = m[0] * c[0] + m[1] * c[4] + m[2] * c[8] + m[3] * c[12];
    if (det_out) *det_out = det;
    if (det == 0.0f) return 0;
    for (int i = 0; i < 16; i++) o[i] = c[i] / det;
    return 1;
}
static void vxm(float *o, const float *v, const float *m) {
    float t[4];
    for (int j = 0; j < 4; j++) t[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, t, sizeof t);
}
static void normalize(float *v, int n) {
    float s = 0;
    for (int i = 0; i < n; i++) s += v[i] * v[i];
    s = sqrtf(s);
    for (int i = 0; i < n; i++) v[i] = s ? v[i] / s : 0.0f;
}
static void quat_matrix(float *m, const float *q) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    ident(m);
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + z * w), m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w), m[9] = 2 * (y * z - x * w), m[10] = 1 - 2 * (x * x + y * y);
}
static void translation(float *m, float x, float y, float z) { ident(m); m[12] = x, m[13] = y, m[14] = z; }

/* ---- matrices ---- */
static void x_MatrixMultiply(void) {              /* (out, m1, m2) */
    float a[16], b[16];
    get(a, A32(1), 16), get(b, A32(2), 16);
    mmul(a, a, b);
    put(A32(0), a, 16);
    RET(A32(0), 3);
}
static void x_MatrixTranspose(void) {             /* (out, m) */
    float a[16], t[16];
    get(a, A32(1), 16);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t[j * 4 + i] = a[i * 4 + j];
    put(A32(0), t, 16);
    RET(A32(0), 2);
}
static void x_MatrixDeterminant(void) {           /* (m): FLOAT in st(0) */
    float a[16];
    get(a, A32(0), 16);
    fp_push_impl(g_st, &g_fp_top, (double)determinant(a));
    RET(0, 1);
}
static void x_MatrixInverse(void) {               /* (out, float* det, m): NULL when singular */
    float a[16], o[16], det;
    get(a, A32(2), 16);
    int ok = inverse(o, &det, a);
    if (A32(1)) put(A32(1), &det, 1);
    if (!ok) RET(0, 3);
    put(A32(0), o, 16);
    RET(A32(0), 3);
}
static void x_MatrixPerspectiveFovLH(void) {      /* (out, fovy, aspect, zn, zf) */
    float fov = F(1), aspect = F(2), zn = F(3), zf = F(4), m[16] = { 0 };
    float ys = 1.0f / tanf(fov / 2), xs = ys / aspect, q = zf / (zf - zn);
    m[0] = xs, m[5] = ys, m[10] = q, m[11] = 1, m[14] = -zn * q;
    put(A32(0), m, 16);
    RET(A32(0), 5);
}
static void x_MatrixOrthoLH(void) {               /* (out, w, h, zn, zf) */
    float w = F(1), h = F(2), zn = F(3), zf = F(4), m[16];
    ident(m);
    m[0] = 2 / w, m[5] = 2 / h, m[10] = 1 / (zf - zn), m[14] = zn / (zn - zf);
    put(A32(0), m, 16);
    RET(A32(0), 5);
}
static void x_MatrixOrthoOffCenterLH(void) {      /* (out, l, r, b, t, zn, zf) */
    float l = F(1), r = F(2), b = F(3), t = F(4), zn = F(5), zf = F(6), m[16];
    ident(m);
    m[0] = 2 / (r - l), m[5] = 2 / (t - b), m[10] = 1 / (zf - zn);
    m[12] = (l + r) / (l - r), m[13] = (t + b) / (b - t), m[14] = zn / (zn - zf);
    put(A32(0), m, 16);
    RET(A32(0), 7);
}
static void x_MatrixScaling(void) {               /* (out, x, y, z) */
    float m[16];
    ident(m);
    m[0] = F(1), m[5] = F(2), m[10] = F(3);
    put(A32(0), m, 16);
    RET(A32(0), 4);
}
static void x_MatrixTranslation(void) {           /* (out, x, y, z) */
    float m[16];
    translation(m, F(1), F(2), F(3));
    put(A32(0), m, 16);
    RET(A32(0), 4);
}
static void rotation(int axis) {                  /* (out, angle) */
    float c = cosf(F(1)), s = sinf(F(1)), m[16];
    ident(m);
    if (axis == 0) m[5] = c, m[6] = s, m[9] = -s, m[10] = c;
    else if (axis == 1) m[0] = c, m[2] = -s, m[8] = s, m[10] = c;
    else m[0] = c, m[1] = s, m[4] = -s, m[5] = c;
    put(A32(0), m, 16);
    RET(A32(0), 2);
}
static void x_MatrixRotationX(void) { rotation(0); }
static void x_MatrixRotationY(void) { rotation(1); }
static void x_MatrixRotationZ(void) { rotation(2); }
static void x_MatrixLookAtLH(void) {              /* (out, eye, at, up) */
    float e[3], a[3], u[3], x[3], y[3], z[3], m[16];
    get(e, A32(1), 3), get(a, A32(2), 3), get(u, A32(3), 3);
    for (int i = 0; i < 3; i++) z[i] = a[i] - e[i];
    normalize(z, 3);
    x[0] = u[1] * z[2] - u[2] * z[1], x[1] = u[2] * z[0] - u[0] * z[2], x[2] = u[0] * z[1] - u[1] * z[0];
    normalize(x, 3);
    y[0] = z[1] * x[2] - z[2] * x[1], y[1] = z[2] * x[0] - z[0] * x[2], y[2] = z[0] * x[1] - z[1] * x[0];
    ident(m);
    for (int i = 0; i < 3; i++) m[i * 4] = x[i], m[i * 4 + 1] = y[i], m[i * 4 + 2] = z[i];
    m[12] = -(x[0] * e[0] + x[1] * e[1] + x[2] * e[2]);
    m[13] = -(y[0] * e[0] + y[1] * e[1] + y[2] * e[2]);
    m[14] = -(z[0] * e[0] + z[1] * e[1] + z[2] * e[2]);
    put(A32(0), m, 16);
    RET(A32(0), 4);
}
static void x_MatrixRotationQuaternion(void) {    /* (out, q) */
    float q[4], m[16];
    get(q, A32(1), 4);
    quat_matrix(m, q);
    put(A32(0), m, 16);
    RET(A32(0), 2);
}
/* (out, scaling, rotation centre, rotation, translation): Ms * Mrc^-1 * Mr * Mrc * Mt, NULLs are identity */
static void x_MatrixAffineTransformation(void) {
    float s = F(1), m[16], t[16];
    ident(m);
    m[0] = m[5] = m[10] = s;
    if (A32(3)) {
        float c[3] = { 0, 0, 0 }, q[4];
        if (A32(2)) get(c, A32(2), 3);
        get(q, A32(3), 4);
        translation(t, -c[0], -c[1], -c[2]), mmul(m, m, t);
        quat_matrix(t, q), mmul(m, m, t);
        translation(t, c[0], c[1], c[2]), mmul(m, m, t);
    }
    if (A32(4)) {
        float v[3];
        get(v, A32(4), 3);
        m[12] += v[0], m[13] += v[1], m[14] += v[2];
    }
    put(A32(0), m, 16);
    RET(A32(0), 5);
}

/* ---- vectors ---- */
static void vnorm(int n) {                        /* (out, v) */
    float v[4];
    get(v, A32(1), n);
    normalize(v, n);
    put(A32(0), v, n);
    RET(A32(0), 2);
}
static void x_Vec2Normalize(void) { vnorm(2); }
static void x_Vec3Normalize(void) { vnorm(3); }
static void x_Vec4Normalize(void) { vnorm(4); }
static void transform_coord(float *o, const float *v3, const float *m) {
    float v[4] = { v3[0], v3[1], v3[2], 1 }, r[4];
    vxm(r, v, m);
    float w = r[3] ? r[3] : 1;                    /* a zero w leaves the point as is (no infinities) */
    o[0] = r[0] / w, o[1] = r[1] / w, o[2] = r[2] / w;
}
static void x_Vec3TransformCoord(void) {          /* (out, v3, m) */
    float v[3], m[16], o[3];
    get(v, A32(1), 3), get(m, A32(2), 16);
    transform_coord(o, v, m);
    put(A32(0), o, 3);
    RET(A32(0), 3);
}
static void x_Vec3Transform(void) {               /* (out vec4, v3, m) */
    float v[4] = { 0, 0, 0, 1 }, m[16], o[4];
    get(v, A32(1), 3), get(m, A32(2), 16);
    vxm(o, v, m);
    put(A32(0), o, 4);
    RET(A32(0), 3);
}
static void x_Vec4Transform(void) {               /* (out, v4, m) */
    float v[4], m[16], o[4];
    get(v, A32(1), 4), get(m, A32(2), 16);
    vxm(o, v, m);
    put(A32(0), o, 4);
    RET(A32(0), 3);
}
/* world * view * projection, any of them NULL (identity) */
static void wvp(float *m, uint32_t p, uint32_t v, uint32_t w) {
    float t[16];
    ident(m);
    if (w) get(t, w, 16), mmul(m, m, t);
    if (v) get(t, v, 16), mmul(m, m, t);
    if (p) get(t, p, 16), mmul(m, m, t);
}
static void viewport(uint32_t va, float *x, float *y, float *w, float *h, float *mn, float *mx) {
    *x = (float)MEM32(va), *y = (float)MEM32(va + 4), *w = (float)MEM32(va + 8), *h = (float)MEM32(va + 12);
    get(mn, va + 16, 1), get(mx, va + 20, 1);
}
static void x_Vec3Project(void) {                 /* (out, v, viewport, projection, view, world) */
    float v[3], m[16], o[3];
    get(v, A32(1), 3);
    wvp(m, A32(3), A32(4), A32(5));
    transform_coord(o, v, m);
    if (A32(2)) {
        float x, y, w, h, mn, mx;
        viewport(A32(2), &x, &y, &w, &h, &mn, &mx);
        o[0] = x + (1 + o[0]) * w / 2, o[1] = y + (1 - o[1]) * h / 2, o[2] = mn + o[2] * (mx - mn);
    }
    put(A32(0), o, 3);
    RET(A32(0), 6);
}
static void x_Vec3Unproject(void) {               /* (out, v, viewport, projection, view, world) */
    float v[3], m[16], inv[16], o[3];
    get(v, A32(1), 3);
    wvp(m, A32(3), A32(4), A32(5));
    inverse(inv, NULL, m);
    if (A32(2)) {
        float x, y, w, h, mn, mx;
        viewport(A32(2), &x, &y, &w, &h, &mn, &mx);
        v[0] = 2 * (v[0] - x) / w - 1, v[1] = 1 - 2 * (v[1] - y) / h, v[2] = (v[2] - mn) / (mx - mn);
    }
    transform_coord(o, v, inv);
    put(A32(0), o, 3);
    RET(A32(0), 6);
}

/* ---- quaternions (x, y, z, w) ---- */
static void x_QuaternionMultiply(void) {          /* (out, q1, q2): the rotation q1 then q2 */
    float a[4], b[4], o[4];
    get(a, A32(1), 4), get(b, A32(2), 4);
    o[0] = b[3] * a[0] + b[0] * a[3] + b[1] * a[2] - b[2] * a[1];
    o[1] = b[3] * a[1] - b[0] * a[2] + b[1] * a[3] + b[2] * a[0];
    o[2] = b[3] * a[2] + b[0] * a[1] - b[1] * a[0] + b[2] * a[3];
    o[3] = b[3] * a[3] - b[0] * a[0] - b[1] * a[1] - b[2] * a[2];
    put(A32(0), o, 4);
    RET(A32(0), 3);
}
static void x_QuaternionInverse(void) {           /* (out, q): the conjugate over the squared length */
    float q[4];
    get(q, A32(1), 4);
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    float o[4] = { -q[0] / n, -q[1] / n, -q[2] / n, q[3] / n };
    put(A32(0), o, 4);
    RET(A32(0), 2);
}
static void x_QuaternionNormalize(void) { vnorm(4); }
static void x_QuaternionRotationYawPitchRoll(void) {   /* (out, yaw, pitch, roll) */
    float sy = sinf(F(1) / 2), cy = cosf(F(1) / 2), sp = sinf(F(2) / 2), cp = cosf(F(2) / 2);
    float sr = sinf(F(3) / 2), cr = cosf(F(3) / 2);
    float o[4] = { sy * cp * sr + cy * sp * cr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
                   cy * cp * cr + sy * sp * sr };
    put(A32(0), o, 4);
    RET(A32(0), 4);
}
static void x_QuaternionSlerp(void) {             /* (out, q1, q2, t): the short way; a lerp when close */
    float a[4], b[4], t = F(3), o[4];
    get(a, A32(1), 4), get(b, A32(2), 4);
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s0 = 1 - t, s1 = t;
    if (d < 0) s1 = -s1, d = -d;
    if (1 - d > 0.001f) {
        float th = acosf(d), sn = sinf(th);
        s0 = sinf(th * s0) / sn, s1 = sinf(th * s1) / sn;
    }
    for (int i = 0; i < 4; i++) o[i] = s0 * a[i] + s1 * b[i];
    put(A32(0), o, 4);
    RET(A32(0), 4);
}
static void x_QuaternionRotationMatrix(void) {    /* (out, m) */
    float m[16], q[4];
    get(m, A32(1), 16);
#define M(r, c) m[(r) * 4 + (c)]
    float tr = M(0, 0) + M(1, 1) + M(2, 2);
    if (tr > 0) {
        float s = 2 * sqrtf(tr + 1);
        q[0] = (M(1, 2) - M(2, 1)) / s, q[1] = (M(2, 0) - M(0, 2)) / s, q[2] = (M(0, 1) - M(1, 0)) / s, q[3] = s / 4;
    } else {
        int i = M(1, 1) > M(0, 0) ? 1 : 0;
        if (M(2, 2) > M(i, i)) i = 2;
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = 2 * sqrtf(1 + M(i, i) - M(j, j) - M(k, k));
        q[i] = s / 4, q[j] = (M(i, j) + M(j, i)) / s, q[k] = (M(i, k) + M(k, i)) / s, q[3] = (M(j, k) - M(k, j)) / s;
    }
#undef M
    put(A32(0), q, 4);
    RET(A32(0), 2);
}

/* ---- halves ---- */
static float half(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (e == 0) {
        if (!m) u = s;
        else {                                    /* a denormal half is a normal float */
            float f = ldexpf((float)m, -24);
            memcpy(&u, &f, 4);
            u |= s;
        }
    } else if (e == 31) u = s | 0x7F800000u | (m << 13);
    else u = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void x_Float16To32Array(void) {            /* (out, in, n) */
    uint32_t o = A32(0), in = A32(1), n = A32(2);
    for (uint32_t i = 0; i < n; i++) {
        float f = half(MEM16(in + 2 * i));
        put(o + 4 * i, &f, 1);
    }
    RET(o, 3);
}

static const win32hle_shim g_math[] = {
    { "D3DXMatrixMultiply", x_MatrixMultiply }, { "D3DXMatrixTranspose", x_MatrixTranspose },
    { "D3DXMatrixDeterminant", x_MatrixDeterminant }, { "D3DXMatrixInverse", x_MatrixInverse },
    { "D3DXMatrixPerspectiveFovLH", x_MatrixPerspectiveFovLH }, { "D3DXMatrixOrthoLH", x_MatrixOrthoLH },
    { "D3DXMatrixOrthoOffCenterLH", x_MatrixOrthoOffCenterLH }, { "D3DXMatrixScaling", x_MatrixScaling },
    { "D3DXMatrixTranslation", x_MatrixTranslation }, { "D3DXMatrixRotationX", x_MatrixRotationX },
    { "D3DXMatrixRotationY", x_MatrixRotationY }, { "D3DXMatrixRotationZ", x_MatrixRotationZ },
    { "D3DXMatrixLookAtLH", x_MatrixLookAtLH }, { "D3DXMatrixRotationQuaternion", x_MatrixRotationQuaternion },
    { "D3DXMatrixAffineTransformation", x_MatrixAffineTransformation },
    { "D3DXVec2Normalize", x_Vec2Normalize }, { "D3DXVec3Normalize", x_Vec3Normalize },
    { "D3DXVec4Normalize", x_Vec4Normalize }, { "D3DXVec3TransformCoord", x_Vec3TransformCoord },
    { "D3DXVec3Transform", x_Vec3Transform }, { "D3DXVec4Transform", x_Vec4Transform },
    { "D3DXVec3Project", x_Vec3Project }, { "D3DXVec3Unproject", x_Vec3Unproject },
    { "D3DXQuaternionMultiply", x_QuaternionMultiply }, { "D3DXQuaternionInverse", x_QuaternionInverse },
    { "D3DXQuaternionNormalize", x_QuaternionNormalize },
    { "D3DXQuaternionRotationYawPitchRoll", x_QuaternionRotationYawPitchRoll },
    { "D3DXQuaternionSlerp", x_QuaternionSlerp }, { "D3DXQuaternionRotationMatrix", x_QuaternionRotationMatrix },
    { "D3DXFloat16To32Array", x_Float16To32Array },
    { 0, 0 } };
void d3dx_math_register(void) { win32hle_register(g_math); }
