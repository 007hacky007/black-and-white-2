"""Plan 5 Tasks 2-4: the D3DX shader assembler, the bytecode decoder, and vertex and pixel shaders on
Metal (runtime/bw2/d3d9/sm_selftest.c).

Every shader here is written by the test. The expected tokens follow the D3D9 token format
(version, instruction, destination and source parameter tokens). The game's own assembly files
and effect bytecode are read only with BW2_GAMEFILES=1, into tmp_path, never into the repo.
Each test runs one group of the selftest headless and expects "<group>: 0 failed"."""
import collections
import hashlib
import os
import struct
import subprocess

import pytest

from bw2 import paths
from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("shaders") / "sm_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "d3d9" / "sm_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    return subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=300,
                          cwd=tmp_path, env=env)


def check(exe, group, tmp_path, *args):
    r = run_group(exe, group, tmp_path, *args)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-6000:] + r.stderr[-4000:]
    return r


def toks(s):
    return [int(t, 16) for t in s.split()]


# name -> (source, expected tokens)
CASES = {
    "vs11_m4x4": ("vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\n",
                  "FFFE0101 0000001F 80000000 900F0000 00000014 C00F0000 90E40000 A0E40000 0000FFFF"),
    "ps20_texld": ("ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n",
                   "FFFF0200 0200001F 80000000 B0030000 0200001F 90000000 A00F0800"
                   " 03000042 800F0000 B0E40000 A0E40800 02000001 800F0800 80E40000 0000FFFF"),
    "ps11_shift_bx2": ("ps_1_1\ntex t0\nmul_x2 r0, t0, v0_bx2\n",
                       "FFFF0101 00000042 B00F0000 00000005 810F0000 B0E40000 94E40000 0000FFFF"),
    # dot headers, def, a0 written by mov, vs_1_1 relative addressing (no extra token), a short swizzle
    "vs11_rel": ("vs.1.1\r\ndcl_position v0\r\ndef c5, 1.0, -0.5, 0, 2\r\nmov a0.x, v0.x\r\n"
                 "mov r0, c[a0.x + 5]\r\nmov oPos, -r0.xz\r\n",
                 "FFFE0101 0000001F 80000000 900F0000 00000051 A00F0005 3F800000 BF000000 00000000 40000000"
                 " 00000001 B0010000 90000000 00000001 800F0000 A0E42005 00000001 C00F0000 81A80000 0000FFFF"),
    # vs_2_0: instruction lengths, the relative-address token, mova, sincos, defi/defb, static flow control
    "vs20_flow": ("VS_2_0\ndcl_position v0\ndcl_texcoord1 v1\ndefi i0, 3, 0, 1, 0\ndefb b0, true\n"
                  "mova a0.x, v1.x\nmov r0, c[a0.x + 2]\nsincos r1.xy, r0.x, c0, c1\nloop aL, i0\n"
                  "add r0, r0, c[aL + 4]\nendloop\nif b0\nmov r0, v0\nelse\nnrm r0.xyz, v1\nendif\n"
                  "mov oPos, r0\nmov oT0.xy, r1\n",
                  "FFFE0200 0200001F 80000000 900F0000 0200001F 80010005 900F0001"
                  " 05000030 F00F0000 00000003 00000000 00000001 00000000 0200002F E00F0800 00000001"
                  " 0200002E B0010000 90000001 03000001 800F0000 A0E42002 B0000000"
                  " 04000025 80030001 80000000 A0E40000 A0E40001 0200001B F0E40800 F0E40000"
                  " 04000002 800F0000 80E40000 A0E42004 F0E40800 0000001D 01000028 E0E40800"
                  " 02000001 800F0000 90E40000 0000002A 02000024 80070000 90E40001 0000002B"
                  " 02000001 C00F0000 80E40000 02000001 E0030000 80E40001 0000FFFF"),
    # ps_1_4: texcrd, texld, phase, co-issue, cnd, shifts, comp, bias, x2 and a replicate swizzle
    "ps14_phase": ("ps.1.4\ndef c0, 0.5, 0.25, 1, 0\ntexcrd r0.xyz, t0\ntexld r1, t1\nphase\ntexld r2, r0\n"
                   "mul_x4 r0.rgb, r1, 1-r2\n+mov_d2 r0.a, -c0_bias\ncnd_sat r0, r0, r1_x2, r2.b\n",
                   "FFFF0104 00000051 A00F0000 3F000000 3E800000 3F800000 00000000 00000040 80070000 B0E40000"
                   " 00000042 800F0001 B0E40001 0000FFFD 00000042 800F0002 80E40000"
                   " 00000005 82070000 80E40001 86E40002 40000001 8F080000 A3E40000"
                   " 00000050 801F0000 80E40000 87E40001 80AA0002 0000FFFF"),
    # ps_2_0: _pp, _centroid, sampler types, texldp, texkill, dp2add, oDepth
    "ps20_mods": ("ps_2_0\ndcl_pp t0\ndcl_centroid t1.xy\ndcl v0\ndcl_cube s0\ndcl_volume s1\n"
                  "texldp r0, t0, s0\ntexld_pp r1, t0, s1\ntexkill t1\ndp2add_sat_pp r2.x, r0, r1, c0.w\n"
                  "mov oC0, r2.x\nmov oDepth, r0.z\n",
                  "FFFF0200 0200001F 80000000 B02F0000 0200001F 80000000 B0430001 0200001F 80000000 900F0000"
                  " 0200001F 98000000 A00F0800 0200001F A0000000 A00F0801 03010042 800F0000 B0E40000 A0E40800"
                  " 03000042 802F0001 B0E40000 A0E40801 01000041 B00F0001"
                  " 0400005A 80310002 80E40000 80E40001 A0FF0000 02000001 800F0800 80000002"
                  " 02000001 900F0800 80AA0000 0000FFFF"),
    # comments and the preprocessor
    "preprocessor": ("// leading comment\n#define SCALE c1\n#ifdef SCALE\nvs_1_1 ; version\n#else\nvs_2_0\n"
                     "#endif\n#if defined(SCALE) && 0\nmov r0, v9\n#elif 2 > 1\n/* block\n   comment */"
                     " dcl_position v0\n#endif\n#ifndef NOPE\nmul oPos, v0, SCALE // trailing\n#endif\n"
                     "#undef SCALE\n#ifdef SCALE\nbogus\n#endif\n",
                     "FFFE0101 0000001F 80000000 900F0000 00000005 C00F0000 90E40000 A0E40001 0000FFFF"),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_asm_tokens(selftest, tmp_path, name):
    src, want = CASES[name]
    f = tmp_path / f"{name}.txt"
    f.write_bytes(src.encode())
    r = check(selftest, "asm_tokens", tmp_path, f)
    line = next(ln for ln in r.stdout.splitlines() if ln.startswith("tokens:"))
    got = toks(line[len("tokens:"):])
    assert [f"{t:08X}" for t in got] == [f"{t:08X}" for t in toks(want)]


def test_asm_errors(selftest, tmp_path):
    check(selftest, "asm_errors", tmp_path)


def test_decode_roundtrip(selftest, tmp_path):
    d = tmp_path / "src"
    d.mkdir()
    for name, (src, _) in CASES.items():
        (d / f"{name}.txt").write_bytes(src.encode())
    r = check(selftest, "decode_roundtrip", tmp_path, d)
    assert r.stdout.count("roundtrip ok") == len(CASES), r.stdout[-4000:]


def test_profiles(selftest, tmp_path):
    check(selftest, "profiles", tmp_path)


def test_asm_include_is_loud(selftest, tmp_path):
    r = run_group(selftest, "asm_loud", tmp_path, "include")
    assert r.returncode != 0 and "asm_loud: returned" not in r.stdout, r.stdout[-2000:]
    assert "FATAL: d3d9: " in r.stderr and '#include "common.vsh"' in r.stderr, r.stderr[-2000:]


@pytest.mark.gamefiles
def test_asm_game_files(selftest, tmp_path):
    # The 29 .vsh/.psh files under Data/EncryptedShaders, each byte shifted back by 3 as the game does.
    src = paths.GAMEFILES / "game" / "Data" / "EncryptedShaders"
    d = tmp_path / "plain"
    d.mkdir()
    files = sorted(p for p in src.iterdir() if p.name.endswith((".vsh.caesar3", ".psh.caesar3")))
    assert len(files) == 29
    for p in files:
        (d / p.name[:-len(".caesar3")]).write_bytes(bytes((b - 3) & 0xFF for b in p.read_bytes()))
    r = check(selftest, "asm_game_files", tmp_path, d)
    results = dict(ln.split(": ", 1) for ln in r.stdout.splitlines()
                   if ln.split(": ", 1)[0].endswith((".vsh", ".psh")))
    assert len(results) == 29, r.stdout[-4000:]
    versions = collections.Counter(v.split()[0] for v in results.values())
    print(dict(versions))
    # Measured: vs.1.1 11, vs.2.0 5, ps.2.0 9, ps.1.1 1, ps.1.4 1, and two without a version line.
    # Those two are HLSL source (D3DXCompileShader's, plan 7), so the assembler rejects them at line 1.
    assert versions == {"vs_1_1": 11, "vs_2_0": 5, "ps_2_0": 9, "ps_1_1": 1, "ps_1_4": 1, "E_FAIL": 2}, results
    for name, v in results.items():
        if v.startswith("E_FAIL"):
            assert "(1): error: expected a shader version" in v, (name, v)
        else:
            assert "roundtrip ok" in v, (name, v)


def _effect_shaders():
    """The distinct size-prefixed shader token streams inside the game's fx_2_0 effects."""
    blobs = {}
    for p in sorted((paths.GAMEFILES / "game" / "Data" / "effects").glob("*.sdv")):
        d = p.read_bytes()
        b = d[d.find(b"\x01\x09\xff\xfe"):]
        for o in range(4, len(b) - 8, 4):
            v, = struct.unpack_from("<I", b, o)
            if v >> 16 not in (0xFFFE, 0xFFFF) or (v & 0xFFFF) not in (0x101, 0x102, 0x103, 0x104, 0x200):
                continue
            n, = struct.unpack_from("<I", b, o - 4)
            if n % 4 == 0 and o + n <= len(b) and struct.unpack_from("<I", b, o + n - 4)[0] == 0xFFFF:
                blobs[hashlib.sha1(b[o:o + n]).hexdigest()] = b[o:o + n]
    return blobs


@pytest.mark.gamefiles
def test_decode_game_effect_shaders(selftest, tmp_path):
    # Every shader in the effects decodes, and printing it as assembly and assembling that again
    # gives the same tokens (comment chunks aside).
    d = tmp_path / "blobs"
    d.mkdir()
    blobs = _effect_shaders()
    assert len(blobs) > 200
    for h, b in blobs.items():
        (d / f"{h}.bin").write_bytes(b)
    r = check(selftest, "decode_files", tmp_path, d)
    assert r.stdout.count("roundtrip ok") == len(blobs), r.stdout[-4000:]


# ---- Task 3: vertex shaders on Metal (pixel tests on a headless 64x64 device) ----

@pytest.mark.parametrize("group", ["vs_basic", "vs_ops", "vs_reladdr", "vs_flow", "vs_inputs", "vs_halfpixel",
                                   "vs_clip", "caps_sm2", "shader_objects", "vs_cache"])
def test_vertex_shaders(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


def test_vs30_is_loud(selftest, tmp_path):
    r = run_group(selftest, "vs_loud", tmp_path)
    assert r.returncode != 0 and "vs_loud: returned" not in r.stdout, r.stdout[-2000:]
    assert "FATAL: d3d9: IDirect3DDevice9::CreateVertexShader: vs_3_0" in r.stderr, r.stderr[-2000:]


# ---- Task 4: pixel shaders on Metal ----

@pytest.mark.parametrize("group", ["ps_ops", "ps_modifiers", "ps_tex", "ps_bump", "ps_fog_alpha", "mixed_stages",
                                   "ps_srgb", "ps_objects"])
def test_pixel_shaders(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


def test_ps30_is_loud(selftest, tmp_path):
    r = run_group(selftest, "ps_loud", tmp_path)
    assert r.returncode != 0 and "ps_loud: returned" not in r.stdout, r.stdout[-2000:]
    assert "FATAL: d3d9: IDirect3DDevice9::CreatePixelShader: ps_3_0" in r.stderr, r.stderr[-2000:]


@pytest.mark.gamefiles
def test_game_shaders_compile(selftest, tmp_path):
    # Every distinct shader in the 59 effects decodes, translates and compiles with Metal.
    d = tmp_path / "blobs"
    d.mkdir()
    blobs = _effect_shaders()
    for h, b in blobs.items():
        (d / f"{h}.bin").write_bytes(b)
    r = check(selftest, "compile_files", tmp_path, d)
    got = collections.Counter(ln.split()[1] for ln in r.stdout.splitlines() if ln.endswith(" compiled"))
    print(dict(got))
    assert sum(got.values()) == len(blobs), r.stdout[-4000:]
    # Measured here (distinct blobs): the plan's "Measured" counts (vs_1_1 22, vs_2_0 20, ps_1_1 17, ps_1_4 4,
    # ps_2_0 19) do not come out of the size-checked scan; Task 2's ruling has these, ps_1_3 included.
    assert got == {"vs_1_1": 67, "vs_2_0": 44, "ps_1_1": 58, "ps_1_3": 1, "ps_1_4": 8, "ps_2_0": 79}, got
