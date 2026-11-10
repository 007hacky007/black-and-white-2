import subprocess

import pytest

from bw2 import build as build_mod
from bw2.build import BuildError, ToolsMissing, build, check_tools
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
