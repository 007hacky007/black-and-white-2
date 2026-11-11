"""Plan 4: the D3D9 layer on Metal, driven as lifted code drives it (runtime/bw2/d3d9/d3d9_selftest.c).

Each test runs one group of the selftest headless and expects "<group>: 0 failed"."""
import os
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("d3d9") / "d3d9_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "d3d9" / "d3d9_selftest.c"]
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


# ---- Task 3: COM objects, IDirect3D9, DirectDraw 7 ----

def test_adapter(selftest, tmp_path):
    check(selftest, "adapter", tmp_path)


def test_ddraw(selftest, tmp_path):
    check(selftest, "ddraw", tmp_path)


def test_many_objects(selftest, tmp_path):
    check(selftest, "objects", tmp_path)


def test_unimplemented_method_is_loud(selftest, tmp_path):
    r = run_group(selftest, "unimplemented", tmp_path)
    assert r.returncode != 0, r.stdout
    assert "IDirect3DDevice9::DrawRectPatch not implemented" in r.stderr, r.stderr[-2000:]


# ---- Task 4: device, swap chain, Present ----

@pytest.mark.parametrize("group", ["clear", "persist", "present_scaled", "reset_rules", "defaults", "fpu",
                                   "depth_objects"])
def test_device(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


# ---- Task 5: textures, surfaces, buffers ----

@pytest.mark.parametrize("group", ["lock_roundtrip", "pitches", "dxt_small_mips", "zeroed", "release_while_bound",
                                   "child_refs", "formats_check"])
def test_resources(selftest, tmp_path, group):
    check(selftest, group, tmp_path)
