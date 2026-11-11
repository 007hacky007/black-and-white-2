import subprocess

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
