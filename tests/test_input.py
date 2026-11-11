"""Plan 5 Task 1: a minimal DirectInput mouse, driven as lifted code drives it (runtime/bw2/input/input_selftest.c).

It answers the game's measured setup and reports no input; plan 6 replaces the reads.
Each test runs one group of the selftest headless and expects "<group>: 0 failed"."""
import os
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)

NOTICE = "[bw2] DirectInput mouse: no input until plan 6"


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("input") / "input_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "input" / "input_selftest.c"]
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
    return r


def test_di_create(selftest, tmp_path):
    r = check(selftest, "di_create", tmp_path)
    assert NOTICE not in r.stderr


def test_di_game_setup(selftest, tmp_path):
    r = check(selftest, "di_game_setup", tmp_path)
    assert r.stderr.count(NOTICE) == 1, r.stderr[-2000:]


def test_di_rules(selftest, tmp_path):
    check(selftest, "di_rules", tmp_path)


@pytest.mark.parametrize("case, message", [
    ("caps", "IDirectInputDevice8A::GetCapabilities not implemented"),
    ("guid", "{6F1D2B61-D5A0-11CF-BFC7-444553540000}"),
    ("coop", "SetCooperativeLevel flags 0x15"),
    ("prop", "SetProperty property 2"),
    ("enum", "IDirectInput8A::EnumDevices not implemented"),
])
def test_di_loud(selftest, tmp_path, case, message):
    r = run_group(selftest, "di_loud", tmp_path, case)
    assert r.returncode != 0 and "di_loud: returned" not in r.stdout, r.stdout[-2000:]
    assert message in r.stderr, r.stderr[-2000:]
    assert "FATAL: d3d9: " in r.stderr, r.stderr[-2000:]
