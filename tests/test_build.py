import re
import subprocess
from pathlib import Path

import pytest

from bw2 import build as build_mod
from functools import partial

from bw2.build import BuildError, ToolsMissing, check_tools

build = partial(build_mod.build, host="linkcheck")   # these tests check the shard pipeline itself
from bw2.lift import ModuleLift, link_tables

SHARD = '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\nvoid {s}(void) {{ g_eax = 1; }}\n'


def _gen(tmp_path):
    g = tmp_path / "gen"
    g.mkdir()
    (g / "w_0000.c").write_text(SHARD.format(s="sub_00401000"))
    (g / "w_0001.c").write_text(SHARD.format(s="sub_00402000"))
    link_tables(g, [ModuleLift("w", 2, 2, 0, {}, ["sub_00401000", "sub_00402000"], [], 0.0)],
                entry=0x401000)
    return g


def test_build_links_and_runs(tmp_path):
    st = build(_gen(tmp_path), tmp_path / "out" / "lc")
    r = subprocess.run([str(st.binary)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "linkcheck: 2 functions, entry 0x00401000 -> found" in r.stdout


def test_broken_shard_fails_with_name(tmp_path):
    g = _gen(tmp_path)
    (g / "w_0001.c").write_text("this is not C\n")
    with pytest.raises(BuildError, match="w_0001.c"):
        build(g, tmp_path / "out" / "lc")
    assert not (tmp_path / "out" / "lc").exists()
    assert not list(g.rglob("*.part"))


def test_rebuild_skips_up_to_date(tmp_path):
    g = _gen(tmp_path)
    build(g, tmp_path / "lc")
    assert build(g, tmp_path / "lc").compiled == 0


def test_rebuild_after_flag_change(tmp_path):
    g = _gen(tmp_path)
    first = build(g, tmp_path / "lc", opt="-O2")
    again = build(g, tmp_path / "lc", opt="-O1")
    assert again.compiled == again.objects == first.objects


def test_bad_opt_rejected(tmp_path):
    g = _gen(tmp_path)
    for bad in ("", "-O9"):
        with pytest.raises(ValueError):
            build(g, tmp_path / "x", opt=bad)


def test_tools_missing(monkeypatch):
    real = subprocess.run

    def fake(cmd, *a, **k):
        if cmd[:2] == ["xcode-select", "-p"]:
            return subprocess.CompletedProcess(cmd, 2, "", "error: no developer tools")
        return real(cmd, *a, **k)
    monkeypatch.setattr(build_mod.subprocess, "run", fake)
    with pytest.raises(ToolsMissing, match="xcode-select --install"):
        check_tools()


def test_lifted_module_compiles_and_links(tmp_path):
    from bw2.lift import lift_module
    from test_lift import _module
    exe, cat = _module(tmp_path, n_extra=2)
    g = tmp_path / "gen"
    m = lift_module("w", exe, cat, g, (".text",))
    link_tables(g, [m], entry=0x401000)
    st = build(g, tmp_path / "lc")
    r = subprocess.run([str(st.binary)], capture_output=True, text=True)
    assert r.returncode == 0 and "4 functions" in r.stdout, r.stdout + r.stderr


def test_cold_shard_compiled_without_optimisation(tmp_path):
    g = _gen(tmp_path)
    plain = build(g, tmp_path / "lc0", opt="-O2").objects
    (g / "w_cold_00403000.c").write_text("#ifdef __OPTIMIZE__\n#error optimised\n#endif\nint w_cold_marker;\n")
    st = build(g, tmp_path / "lc", opt="-O2")
    assert st.objects == plain + 1
    (g / "w_0001.c").write_text("#ifdef __OPTIMIZE__\n#error optimised\n#endif\n" + SHARD.format(s="sub_00402000"))
    with pytest.raises(BuildError, match="w_0001.c"):
        build(g, tmp_path / "lc", opt="-O2")


def test_header_change_recompiles_everything(tmp_path):
    # Review: an edited header (e.g. MEM32 in recomp_types.h) must not reuse stale objects.
    g = _gen(tmp_path)
    first = build(g, tmp_path / "lc")
    h = g / "recomp_funcs.h"
    h.write_text(h.read_text() + "/* changed */\n")
    again = build(g, tmp_path / "lc")
    assert again.compiled == again.objects == first.objects


def test_signed_overflow_wraps_like_x86(tmp_path):
    # Review: lifted imul is (int32_t)a * (int32_t)b; x86 code overflows on purpose
    # (hashes, LCGs), and without -fwrapv clang -O3 may assume it cannot.
    from bw2.build import FLAGS, sdk_path
    src = tmp_path / "w.c"
    src.write_text("#include <limits.h>\n"
                   "__attribute__((noinline)) int gt(int x) { return x + 1 > x; }\n"
                   "int main(void) { return gt(INT_MAX); }\n")
    exe = tmp_path / "w"
    subprocess.run([str(check_tools()), "-O3", *FLAGS, "-isysroot", str(sdk_path()), str(src),
                    "-o", str(exe)], check=True)
    assert subprocess.run([str(exe)]).returncode == 0


# ---- plan 10 Task 5: FFmpeg for the engine host only ----

FFMPEG_LIBS = ("libavformat", "libavcodec", "libavutil")
FFMPEG_NOT = ("libswscale", "libswresample", "libavfilter", "libavdevice", "libpostproc")


def _fake_prefix(root, missing=()):
    for h in ("libavformat/avformat.h", "libavcodec/avcodec.h", "libavutil/avutil.h"):
        (root / "include" / h).parent.mkdir(parents=True, exist_ok=True)
        (root / "include" / h).write_text("")
    (root / "lib").mkdir(exist_ok=True)
    for lib in FFMPEG_LIBS:
        if lib not in missing:
            (root / "lib" / f"{lib}.dylib").write_text("")
    return root


def test_ffmpeg_prefix(tmp_path, monkeypatch):
    from bw2.build import ffmpeg_prefix, engine_libs, HOST_LIBS
    real = Path("/opt/homebrew/opt/ffmpeg")
    monkeypatch.delenv("BW2_FFMPEG_PREFIX", raising=False)
    assert ffmpeg_prefix() == real
    libs = engine_libs()
    assert libs[-4:] == [f"-L{real}/lib", "-lavformat", "-lavcodec", "-lavutil"], libs
    # the environment is read on every call
    fake = _fake_prefix(tmp_path / "ok")
    monkeypatch.setenv("BW2_FFMPEG_PREFIX", str(fake))
    assert ffmpeg_prefix() == fake
    assert f"-L{fake}/lib" in engine_libs()
    broken = _fake_prefix(tmp_path / "broken", missing=("libavcodec",))
    monkeypatch.setenv("BW2_FFMPEG_PREFIX", str(broken))
    with pytest.raises(ToolsMissing, match=r"libavcodec\.dylib"):
        ffmpeg_prefix()
    with pytest.raises(ToolsMissing, match=f"FFmpeg not found under {broken}: brew install ffmpeg, "
                                          "or set BW2_FFMPEG_PREFIX"):
        engine_libs()
    empty = tmp_path / "empty"
    empty.mkdir()
    monkeypatch.setenv("BW2_FFMPEG_PREFIX", str(empty))
    with pytest.raises(ToolsMissing, match="FFmpeg not found"):
        ffmpeg_prefix()
    # importing bw2.build, and a linkcheck build, never need FFmpeg
    import os
    import sys
    r = subprocess.run([sys.executable, "-c", "import bw2.build as b; print(b.HOST_LIBS['linkcheck']())"],
                       capture_output=True, text=True, cwd=build_mod.REPO, env=dict(os.environ))
    assert r.returncode == 0 and r.stdout == "[]\n", r.stdout + r.stderr
    assert build_mod.HOST_LIBS["linkcheck"]() == []
    assert build(_gen(tmp_path), tmp_path / "lc").binary.exists()


@pytest.mark.gamefiles
def test_engine_ffmpeg_links(record_property):
    from bw2 import paths
    engine = paths.REPO / "out" / "bw2"
    assert engine.exists(), "build it first: python -m bw2.pipeline"
    otool = check_tools().parent / "otool"
    r = subprocess.run([str(otool), "-L", str(engine)], capture_output=True, text=True, check=True)
    rows = {}
    for line in r.stdout.splitlines()[1:]:
        m = re.match(r"\s+(\S+) \(compatibility version ([\d.]+), current version ([\d.]+)\)", line)
        assert m, line
        rows[m.group(1)] = (m.group(2), m.group(3))
    ours = {name: v for name, v in rows.items() if any(f"/{lib}." in name for lib in FFMPEG_LIBS)}
    assert sorted(re.search(r"/(lib\w+)\.", n).group(1) for n in ours) == sorted(FFMPEG_LIBS), rows
    assert not [n for n in rows if any(f"/{lib}." in n for lib in FFMPEG_NOT)], rows
    for name, (compat, current) in sorted(ours.items()):
        assert Path(name).exists(), (f"{name} (linked into out/bw2) is missing: a brew upgrade ffmpeg "
                                     "needs a rebuild of out/bw2 (python -m bw2.pipeline --build-only)")
        record_property(Path(name).name, f"compatibility {compat}, current {current}")


# ---- plan 13 Task 3: the LGPL-only FFmpeg and the app's rpath ----

def test_engine_libs_rpath_only_for_rpath_names(fake_ffmpeg_prefix, monkeypatch):
    from bw2.build import engine_libs
    rpath = "-Wl,-rpath,@executable_path/../Frameworks"
    fake = fake_ffmpeg_prefix()
    for h in ("libavformat/avformat.h", "libavcodec/avcodec.h", "libavutil/avutil.h"):
        (fake / "include" / h).parent.mkdir(parents=True, exist_ok=True)
        (fake / "include" / h).write_text("")
    monkeypatch.setenv("BW2_FFMPEG_PREFIX", str(fake))
    libs = engine_libs()
    assert [a for a in libs if a.startswith("-Wl,-rpath")] == [rpath], libs
    absolute = fake_ffmpeg_prefix(install_name=lambda n: f"/opt/homebrew/lib/{n}")
    for h in ("libavformat/avformat.h", "libavcodec/avcodec.h", "libavutil/avutil.h"):
        (absolute / "include" / h).parent.mkdir(parents=True, exist_ok=True)
        (absolute / "include" / h).write_text("")
    monkeypatch.setenv("BW2_FFMPEG_PREFIX", str(absolute))
    assert not [a for a in engine_libs() if a.startswith("-Wl,-rpath")]
    monkeypatch.delenv("BW2_FFMPEG_PREFIX")
    assert not [a for a in engine_libs() if a.startswith("-Wl,-rpath")]   # Homebrew's default


@pytest.mark.gamefiles
def test_engine_rpath_for_lgpl(lgpl_engine):
    otool = check_tools().parent / "otool"
    r = subprocess.run([str(otool), "-L", str(lgpl_engine)], capture_output=True, text=True, check=True)
    names = [l.strip().split(" ")[0] for l in r.stdout.splitlines()[1:] if l.strip()]
    ours = [n for n in names if any(f"/{lib}." in n for lib in FFMPEG_LIBS)]
    assert sorted(ours) == ["@rpath/libavcodec.63.dylib", "@rpath/libavformat.63.dylib",
                            "@rpath/libavutil.61.dylib"], names
    assert not [n for n in names if any(f"/{lib}." in n for lib in FFMPEG_NOT)], names
    assert not [n for n in names if n.startswith("/opt/homebrew")], names
    load = subprocess.run([str(otool), "-l", str(lgpl_engine)], capture_output=True, text=True,
                          check=True).stdout
    rpaths = re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (\S+) \(offset", load)
    assert rpaths == ["@executable_path/../Frameworks"], rpaths
    assert (lgpl_engine.parent / "d3dx9_25.dll").is_file()
