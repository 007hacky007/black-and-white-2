"""Plan 7: D3DX's 2D layer - host calls into guest COM (d3d_call), ID3DXSprite and the fonts
(runtime/bw2/d3dx/ui_selftest.c). Pixel groups draw into a 64x64 A8R8G8B8 target, headless.

The font groups use the Mac's own Arial Narrow and Courier New, and copies of them renamed or with a
table dropped (tests/ttf.py), written to tmp_path. Their expectations are worked out here from the same
files, by the rules in plan 7 Task 3."""
import math
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ttf  # noqa: E402

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


def check(exe, group, tmp_path, *args):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    r = subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=300,
                       cwd=tmp_path, env=env)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-6000:] + r.stderr[-4000:]
    return r


@pytest.mark.parametrize("group", ["d3d_call", "sprite_basic", "sprite_texel_exact", "sprite_args", "sprite_sort",
                                   "sprite_state", "sprite_objectspace", "sprite_rules", "sprite_lost"])
def test_ui_group(selftest, group, tmp_path):
    check(selftest, group, tmp_path)


# ---- fonts ----

SUPPLEMENTAL = Path("/System/Library/Fonts/Supplemental")
ARIAL = SUPPLEMENTAL / "Arial Narrow.ttf"
COURIER = SUPPLEMENTAL / "Courier New.ttf"


@pytest.fixture
def fonts():
    for f in (ARIAL, COURIER, ARIAL.with_name("Arial Narrow Bold.ttf")):
        if not f.exists():
            pytest.skip(f"{f} is not on this Mac")
    return ARIAL.read_bytes(), COURIER.read_bytes()


def write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return p


def lines(stdout, prefix):
    """{label: {key: int}} from the selftest's '<prefix> <label> k=v ...' lines."""
    out = {}
    for ln in stdout.splitlines():
        parts = ln.split()
        if len(parts) > 2 and parts[0] == prefix:
            out[parts[1]] = {k: int(v) for k, v in (kv.split("=") for kv in parts[2:])}
    return out


def sources(stdout):
    return dict(re.findall(r"^source (\S+) (.+)$", stdout, re.M))


def test_font_metrics(selftest, tmp_path, fonts):
    arial, courier = fonts
    novdmx = write(tmp_path, "novdmx.ttf", ttf.renamed(ttf.dropped(arial, "VDMX"), "BW2 No VDMX"))
    r = check(selftest, "font_metrics", tmp_path, ARIAL, COURIER, novdmx)
    got, src = lines(r.stdout, "tm"), sources(r.stdout)
    assert src == {"arial": str(ARIAL), "courier": str(COURIER)}
    for h in (-8, -16, -46, 20):
        assert got[f"arial{h}"] == ttf.metrics(arial, h), h
    assert got["courier-16"] == ttf.metrics(courier, -16)
    assert got["courier-16"]["pitch_family"] == 0x36
    assert got["novdmx-16"] == ttf.metrics(ttf.dropped(arial, "VDMX"), -16)


def test_font_advance(selftest, tmp_path, fonts):
    arial, _ = fonts
    nohdmx = ttf.renamed(ttf.dropped(arial, "hdmx"), "BW2 No HDMX")
    r = check(selftest, "font_advance", tmp_path, write(tmp_path, "nohdmx.ttf", nohdmx))
    seen = 0
    for k, data in ((0, arial), (1, nohdmx)):
        ppem = ttf.metrics(data, -16)["ppem"]
        widths = ttf.hdmx(data, ppem)
        assert (widths is None) == (k == 1)
        s = ppem / ttf.units_per_em(data)
        for ch in "AWi ":
            m = re.search(rf"^adv {k} {ord(ch)} glyph=(\d+) advance=(-?\d+)$", r.stdout, re.M)
            assert m, (k, ch)
            g, adv = int(m[1]), int(m[2])
            assert g == ttf.glyph(data, ord(ch))
            assert adv == (widths[g] if widths else ttf.rnd(ttf.advance(data, g) * s)), (k, ch)
            seen += 1
    assert seen == 8


def test_font_memory(selftest, tmp_path, fonts):
    arial, courier = fonts
    bold = ARIAL.with_name("Arial Narrow Bold.ttf").read_bytes()
    check(selftest, "font_memory", tmp_path,
          write(tmp_path, "test_face.ttf", ttf.renamed(arial, "BW2 Test Face")),
          write(tmp_path, "arial_as_courier.ttf", ttf.renamed(arial, "Courier New")),
          ttf.num_glyphs(arial), ttf.num_glyphs(courier),
          write(tmp_path, "two.ttc", ttf.collection(ttf.renamed(arial, "BW2 TTC One"),
                                                    ttf.renamed(courier, "BW2 TTC Two"))),
          write(tmp_path, "bold_face.ttf", ttf.renamed(bold, "BW2 Bold Face")))


def test_font_errors(selftest, tmp_path, fonts):
    arial, _ = fonts
    r = check(selftest, "font_errors", tmp_path, write(tmp_path, "test_face.ttf", ttf.renamed(arial, "BW2 Test Face")))
    assert sources(r.stdout)["arial-bold"] == str(ARIAL.with_name("Arial Narrow Bold.ttf"))
    assert "16 bytes" in r.stderr and "not a TrueType/OpenType font" in r.stderr


def test_font_fallthrough(selftest, tmp_path, fonts):
    arial, courier = fonts
    collide = ttf.renamed(courier, "Arial Narrow", ps=ttf.name(arial, 6))
    r = check(selftest, "font_fallthrough", tmp_path, ARIAL, write(tmp_path, "collide.ttf", collide),
              ttf.num_glyphs(courier))
    assert sources(r.stdout) == {"regular": "memory", "bold": str(ARIAL.with_name("Arial Narrow Bold.ttf")),
                                 "italic": str(ARIAL.with_name("Arial Narrow Italic.ttf")),
                                 "bolditalic": str(ARIAL.with_name("Arial Narrow Bold Italic.ttf"))}


def test_font_raster(selftest, tmp_path, fonts):
    arial, _ = fonts
    s = 46 / ttf.units_per_em(arial)
    x0, y0, x1, y1 = ttf.bbox(arial, ttf.glyph(arial, ord("I")))
    check(selftest, "font_raster", tmp_path, ttf.cap_height(arial) * s, x0 * s, y0 * s, x1 * s, y1 * s)


def test_font_cp1252(selftest, tmp_path):
    check(selftest, "font_cp1252", tmp_path)
