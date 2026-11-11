"""Plan 4: D3DX's math and textures, run as lifted code calls them (runtime/bw2/d3dx/d3dx_selftest.c).

The math checks are written here, in plain Python from the documented formulas, and passed to the
selftest as a file of cases: inputs, then the outputs each function must give."""
import math
import os
import random
import struct
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("d3dx") / "d3dx_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "d3dx" / "d3dx_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    return subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=120,
                          cwd=tmp_path, env=env)


def check(exe, group, tmp_path, *args):
    r = run_group(exe, group, tmp_path, *args)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]


# ---- the reference: D3DX's documented math, row vectors (v' = v * M), M[row][col] ----

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def mat(rows):
    return [list(map(float, r)) for r in rows]


def ident():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def transpose(m):
    return [[m[j][i] for j in range(4)] for i in range(4)]


def det(m):
    def minor(m, i, j):
        return [r[:j] + r[j + 1:] for k, r in enumerate(m) if k != i]

    def d(m):
        if len(m) == 1:
            return m[0][0]
        return sum((-1) ** j * m[0][j] * d(minor(m, 0, j)) for j in range(len(m)))
    return d(m)


def inverse(m):
    n = 4
    a = [r[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, r in enumerate(m)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        pv = a[c][c]
        a[c] = [x / pv for x in a[c]]
        for r in range(n):
            if r != c:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [r[n:] for r in a]


def flat(m):
    return [x for r in m for x in r]


def vxm(v, m):
    return [sum(v[k] * m[k][j] for k in range(4)) for j in range(4)]


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n else [0.0] * len(v)


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def persp_fov_lh(fovy, aspect, zn, zf):
    ys = 1 / math.tan(fovy / 2)
    xs = ys / aspect
    return mat([[xs, 0, 0, 0], [0, ys, 0, 0], [0, 0, zf / (zf - zn), 1], [0, 0, -zn * zf / (zf - zn), 0]])


def ortho_lh(w, h, zn, zf):
    return mat([[2 / w, 0, 0, 0], [0, 2 / h, 0, 0], [0, 0, 1 / (zf - zn), 0], [0, 0, zn / (zn - zf), 1]])


def ortho_offcenter_lh(l, r, b, t, zn, zf):
    return mat([[2 / (r - l), 0, 0, 0], [0, 2 / (t - b), 0, 0], [0, 0, 1 / (zf - zn), 0],
                [(l + r) / (l - r), (t + b) / (b - t), zn / (zn - zf), 1]])


def scaling(x, y, z):
    return mat([[x, 0, 0, 0], [0, y, 0, 0], [0, 0, z, 0], [0, 0, 0, 1]])


def translation(x, y, z):
    return mat([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [x, y, z, 1]])


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return mat([[1, 0, 0, 0], [0, c, s, 0], [0, -s, c, 0], [0, 0, 0, 1]])


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return mat([[c, 0, -s, 0], [0, 1, 0, 0], [s, 0, c, 0], [0, 0, 0, 1]])


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return mat([[c, s, 0, 0], [-s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])


def look_at_lh(eye, at, up):
    z = norm([a - e for a, e in zip(at, eye)])
    x = norm(cross(up, z))
    y = cross(z, x)
    return mat([[x[0], y[0], z[0], 0], [x[1], y[1], z[1], 0], [x[2], y[2], z[2], 0],
                [-dot(x, eye), -dot(y, eye), -dot(z, eye), 1]])


def rot_quat(q):
    x, y, z, w = q
    return mat([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0],
                [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0],
                [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0], [0, 0, 0, 1]])


def affine(s, center, rot, trans):
    m = scaling(s, s, s)
    if rot is not None:
        c = center or [0, 0, 0]
        m = mmul(m, translation(-c[0], -c[1], -c[2]))
        m = mmul(m, rot_quat(rot))
        m = mmul(m, translation(c[0], c[1], c[2]))
    if trans is not None:
        m = mmul(m, translation(*trans))
    return m


def qmul(q1, q2):                          # the rotation q1, then q2 (D3DX: q2 * q1)
    x1, y1, z1, w1 = q1
    x2, y2, z2, w2 = q2
    return [w2 * x1 + x2 * w1 + y2 * z1 - z2 * y1, w2 * y1 - x2 * z1 + y2 * w1 + z2 * x1,
            w2 * z1 + x2 * y1 - y2 * x1 + z2 * w1, w2 * w1 - x2 * x1 - y2 * y1 - z2 * z1]


def qinv(q):
    n = dot(q, q)
    return [-q[0] / n, -q[1] / n, -q[2] / n, q[3] / n]


def qypr(yaw, pitch, roll):
    sy, cy = math.sin(yaw / 2), math.cos(yaw / 2)
    sp, cp = math.sin(pitch / 2), math.cos(pitch / 2)
    sr, cr = math.sin(roll / 2), math.cos(roll / 2)
    return [sy * cp * sr + cy * sp * cr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr]


def qslerp(q1, q2, t):
    d = dot(q1, q2)
    a, b = 1 - t, t
    if d < 0:
        b, d = -b, -d
    if 1 - d > 0.001:
        th = math.acos(d)
        a, b = math.sin(th * a) / math.sin(th), math.sin(th * b) / math.sin(th)
    return [a * x + b * y for x, y in zip(q1, q2)]


def qfrom_matrix(m):                       # the inverse of rot_quat, for a rotation matrix
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = 2 * math.sqrt(tr + 1)
        return [(m[1][2] - m[2][1]) / s, (m[2][0] - m[0][2]) / s, (m[0][1] - m[1][0]) / s, s / 4]
    i = max(range(3), key=lambda k: m[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = 2 * math.sqrt(1 + m[i][i] - m[j][j] - m[k][k])
    q = [0.0] * 4
    q[i] = s / 4
    q[j] = (m[i][j] + m[j][i]) / s
    q[k] = (m[i][k] + m[k][i]) / s
    q[3] = (m[j][k] - m[k][j]) / s
    return q


def transform_coord(v, m):
    o = vxm(list(v) + [1.0], m)
    return [x / o[3] for x in o[:3]]


def project(v, vp, p, view, w):
    m = mmul(mmul(w or ident(), view or ident()), p or ident())
    o = transform_coord(v, m)
    if vp:
        x, y, ww, hh, mn, mx = vp
        o = [x + (1 + o[0]) * ww / 2, y + (1 - o[1]) * hh / 2, mn + o[2] * (mx - mn)]
    return o


def unproject(v, vp, p, view, w):
    m = inverse(mmul(mmul(w or ident(), view or ident()), p or ident()))
    o = list(v)
    if vp:
        x, y, ww, hh, mn, mx = vp
        o = [2 * (v[0] - x) / ww - 1, 1 - 2 * (v[1] - y) / hh, (v[2] - mn) / (mx - mn)]
    return transform_coord(o, m)


def half_to_float(h):
    return struct.unpack("<e", struct.pack("<H", h))[0]


def cases():
    rnd = random.Random(1234)
    r = lambda lo=-2.0, hi=2.0: f32(rnd.uniform(lo, hi))
    rm = lambda: [[r() for _ in range(4)] for _ in range(4)]
    rq = lambda: norm([r() for _ in range(4)])
    out = []

    def add(name, ins, outs):
        out.append(f"{name} in {' '.join(repr(float(x)) for x in ins)} out {' '.join(repr(float(x)) for x in outs)}")

    for _ in range(3):
        a, b = rm(), rm()
        add("D3DXMatrixMultiply", flat(a) + flat(b), flat(mmul(a, b)))
        add("D3DXMatrixMultiply/alias", flat(a) + flat(b), flat(mmul(a, b)))
        add("D3DXMatrixTranspose", flat(a), flat(transpose(a)))
        add("D3DXMatrixTranspose/alias", flat(a), flat(transpose(a)))
        add("D3DXMatrixDeterminant", flat(a), [det(a)])
        add("D3DXMatrixInverse", flat(a), flat(inverse(a)) + [det(a)])
        add("D3DXMatrixInverse/alias", flat(a), flat(inverse(a)))
        fov, asp, zn, zf = r(0.5, 2.0), r(0.5, 2.0), r(0.1, 1.0), r(10, 100)
        add("D3DXMatrixPerspectiveFovLH", [fov, asp, zn, zf], flat(persp_fov_lh(fov, asp, zn, zf)))
        w, h = r(1, 100), r(1, 100)
        add("D3DXMatrixOrthoLH", [w, h, zn, zf], flat(ortho_lh(w, h, zn, zf)))
        l, rr, bb, t = r(-10, -1), r(1, 10), r(-10, -1), r(1, 10)
        add("D3DXMatrixOrthoOffCenterLH", [l, rr, bb, t, zn, zf], flat(ortho_offcenter_lh(l, rr, bb, t, zn, zf)))
        x, y, z = r(), r(), r()
        add("D3DXMatrixScaling", [x, y, z], flat(scaling(x, y, z)))
        add("D3DXMatrixTranslation", [x, y, z], flat(translation(x, y, z)))
        ang = r(-3, 3)
        add("D3DXMatrixRotationX", [ang], flat(rot_x(ang)))
        add("D3DXMatrixRotationY", [ang], flat(rot_y(ang)))
        add("D3DXMatrixRotationZ", [ang], flat(rot_z(ang)))
        eye, at = [r(), r(), r() - 5], [r(), r(), r() + 5]
        up = [0.0, 1.0, 0.0]
        add("D3DXMatrixLookAtLH", eye + at + up, flat(look_at_lh(eye, at, up)))
        q = [f32(c) for c in rq()]
        add("D3DXMatrixRotationQuaternion", q, flat(rot_quat(q)))
        c3, t3, s = [r(), r(), r()], [r(), r(), r()], r(0.5, 2)
        add("D3DXMatrixAffineTransformation", [s] + c3 + q + t3, flat(affine(s, c3, q, t3)))
        add("D3DXMatrixAffineTransformation/nulls", [s] + q, flat(affine(s, None, q, None)))
        v2, v3, v4 = [r(), r()], [r(), r(), r()], [r(), r(), r(), r()]
        add("D3DXVec2Normalize", v2, norm(v2))
        add("D3DXVec3Normalize", v3, norm(v3))
        add("D3DXVec4Normalize", v4, norm(v4))
        m = mmul(rm(), translation(0, 0, 5))
        add("D3DXVec3TransformCoord", v3 + flat(m), transform_coord(v3, m))
        add("D3DXVec3Transform", v3 + flat(m), vxm(v3 + [1.0], m))
        add("D3DXVec4Transform", v4 + flat(m), vxm(v4, m))
        vp = [rnd.randrange(0, 50), rnd.randrange(0, 50), rnd.randrange(100, 800), rnd.randrange(100, 600), 0.0, 1.0]
        pm, vm, wm = persp_fov_lh(1.0, 1.3, 1, 100), look_at_lh([0, 0, -10], [0, 0, 0], [0, 1, 0]), rot_y(ang)
        pv = [r(-2, 2), r(-2, 2), r(-2, 2)]
        add("D3DXVec3Project", pv + vp + flat(pm) + flat(vm) + flat(wm), project(pv, vp, pm, vm, wm))
        add("D3DXVec3Project/nullworld", pv + vp + flat(pm) + flat(vm), project(pv, vp, pm, vm, None))
        # unproject is ill-conditioned in float near the far plane: a far/near of 10, float inputs
        pm2 = persp_fov_lh(1.0, 1.3, 1, 10)
        sp = [f32(c) for c in project(pv, vp, pm2, vm, wm)]
        add("D3DXVec3Unproject", sp + vp + flat(pm2) + flat(vm) + flat(wm), unproject(sp, vp, pm2, vm, wm))
        q1, q2 = [f32(c) for c in rq()], [f32(c) for c in rq()]
        add("D3DXQuaternionMultiply", q1 + q2, qmul(q1, q2))
        add("D3DXQuaternionInverse", q1, qinv(q1))
        add("D3DXQuaternionNormalize", v4, norm(v4))
        y_, p_, ro = r(-3, 3), r(-1.5, 1.5), r(-3, 3)
        add("D3DXQuaternionRotationYawPitchRoll", [y_, p_, ro], qypr(y_, p_, ro))
        tt = r(0, 1)
        add("D3DXQuaternionSlerp", q1 + q2 + [tt], qslerp(q1, q2, tt))
        mq = rot_quat(q1)
        add("D3DXQuaternionRotationMatrix", flat(mq), qfrom_matrix(mq))
    # the edge cases: a singular inverse, a slerp of q with -q, a zero vector
    add("D3DXMatrixInverse/singular", flat([[1, 2, 3, 4], [2, 4, 6, 8], [0, 1, 0, 0], [0, 0, 0, 1]]), [])
    q = norm([0.1, 0.2, 0.3, 0.9])
    add("D3DXQuaternionSlerp", q + [-x for x in q] + [0.5], q)
    add("D3DXVec3Normalize", [0, 0, 0], [0, 0, 0])
    halves = [0x0000, 0x3C00, 0xC000, 0x7BFF, 0x0001, 0x03FF, 0x7C00, 0xFC00, 0x3555, 0x8000]
    add("D3DXFloat16To32Array", halves, [half_to_float(h) for h in halves])
    return out


def test_math(selftest, tmp_path):
    (tmp_path / "cases.txt").write_text("\n".join(cases()) + "\n")
    check(selftest, "math", tmp_path, tmp_path / "cases.txt")
