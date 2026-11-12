"""Plan 7: D3DX's 2D layer - host calls into guest COM (d3d_call) and ID3DXSprite
(runtime/bw2/d3dx/ui_selftest.c). Pixel groups draw into a 64x64 A8R8G8B8 target, headless."""
import os
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("ui") / "ui_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "d3dx" / "ui_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def check(exe, group, tmp_path):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    r = subprocess.run([str(exe), group], capture_output=True, text=True, timeout=300, cwd=tmp_path, env=env)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-6000:] + r.stderr[-4000:]
    return r


@pytest.mark.parametrize("group", ["d3d_call", "sprite_basic", "sprite_texel_exact", "sprite_args", "sprite_sort",
                                   "sprite_state", "sprite_objectspace", "sprite_rules", "sprite_lost"])
def test_ui_group(selftest, group, tmp_path):
    check(selftest, group, tmp_path)
