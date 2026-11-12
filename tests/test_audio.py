"""Plan 8 Task 2: the host mixer and its output (runtime/bw2/audio/audio_selftest.c).

Each test runs one group of the selftest headless and expects "<group>: 0 failed".
The groups put their sample data in hle_alloc blocks; nothing here is game material."""
import os
import re
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)

NOTICE = "[bw2] audio: no output device (OSStatus "
STOPPED = "[bw2] audio: output stopped at exit"


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("audio") / "audio_selftest"
    src = BW2RT / "audio" / "audio_selftest.c"
    assert src.exists(), f"{src} is missing"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [src]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args, headless=True, **env_extra):
    env = dict(os.environ, BW2_SUPPORT=str(tmp_path / "support"))
    env.pop("BW2_AUDIO_LOG", None)
    env.update(env_extra)
    if headless:
        env["BW2_HEADLESS"] = "1"
    else:
        env.pop("BW2_HEADLESS", None)
    return subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=120,
                          cwd=tmp_path, env=env)


def check(exe, group, tmp_path, *args, **kw):
    r = run_group(exe, group, tmp_path, *args, **kw)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    return r


@pytest.mark.parametrize("group", ["ds_mix_exact", "ds_mix_formats", "ds_mix_sum", "ds_mix_cursor"])
def test_ds_mix(selftest, tmp_path, group):
    r = check(selftest, group, tmp_path)
    assert NOTICE not in r.stderr


def test_ds_mix_free(selftest, tmp_path):
    r = check(selftest, "ds_mix_free", tmp_path)
    m = re.search(r"checked (\d+) samples", r.stdout)
    assert m and int(m.group(1)) > 0, r.stdout[-2000:]


def test_ds_out_null(selftest, tmp_path):
    r = check(selftest, "ds_out_null", tmp_path)
    assert r.stderr.count(NOTICE) == 1, r.stderr[-2000:]
    assert ", mixing to the null sink" in r.stderr, r.stderr[-2000:]


def test_ds_out_stall(selftest, tmp_path):
    r = check(selftest, "ds_out_stall", tmp_path)
    assert NOTICE not in r.stderr, r.stderr[-2000:]


@pytest.mark.parametrize("how, stopped", [("hle", 0), ("exit", 1)])
def test_ds_out_exit(selftest, tmp_path, how, stopped):
    r = run_group(selftest, "ds_out_exit", tmp_path, how, BW2_AUDIO_LOG="1")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    m = re.search(r"ds_out_exit: consumed (\d+) frames, leaving by (\w+)", r.stdout)
    assert m and int(m.group(1)) > 0 and m.group(2) == how, r.stdout[-2000:]
    assert r.stderr.count(STOPPED) == stopped, r.stderr[-2000:]


def test_ds_audio_log(selftest, tmp_path):
    r = check(selftest, "ds_audio_log", tmp_path, BW2_AUDIO_LOG="1")
    lines = [ln for ln in r.stderr.splitlines() if "audio second" in ln]
    assert lines == ["[bw2] audio second 1: 1 playing, peak 16383, nonsilent 44100 of 44100",
                     "[bw2] audio second 2: 1 playing, peak 16383, nonsilent 44100 of 44100"], r.stderr[-2000:]


def test_ds_audio_log_off(selftest, tmp_path):
    r = check(selftest, "ds_audio_log", tmp_path)
    assert "audio second" not in r.stderr, r.stderr[-2000:]


@pytest.mark.skipif(os.environ.get("BW2_AUDIO_DEVICE") != "1", reason="set BW2_AUDIO_DEVICE=1 to play on this Mac")
def test_ds_device(selftest, tmp_path):
    r = check(selftest, "ds_device", tmp_path, headless=False)
    assert NOTICE not in r.stderr, r.stderr[-2000:]
