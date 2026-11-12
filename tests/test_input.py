"""Plan 5 Task 1: a minimal DirectInput mouse, driven as lifted code drives it (runtime/bw2/input/input_selftest.c).
Plan 6 Task 1: the host input queue, the Windows messages it makes and IMM32.
Plan 6 Task 2: the DirectInput mouse fed from the host input queue.
Plan 6 Task 3: Mac events translated into the game's terms (translate.c).
Plan 6 Task 5: the cursor policy (cursor.c) and the cursor shims.

Each test runs one group of the selftest headless and expects "<group>: 0 failed"."""
import os
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)

FIRST_READ = "[bw2] DirectInput mouse: first read"


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("input") / "input_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "input" / "input_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args, quit_after=None):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    env.pop("BW2_QUIT_AFTER_READS", None)
    if quit_after is not None:
        env["BW2_QUIT_AFTER_READS"] = quit_after
    return subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=120,
                          cwd=tmp_path, env=env)


def check(exe, group, tmp_path, *args):
    r = run_group(exe, group, tmp_path, *args)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    return r


def test_di_create(selftest, tmp_path):
    r = check(selftest, "di_create", tmp_path)
    assert FIRST_READ not in r.stderr
    assert "no input until plan 6" not in r.stderr


def test_di_game_setup(selftest, tmp_path):
    r = check(selftest, "di_game_setup", tmp_path)
    assert r.stderr.count(FIRST_READ) == 1, r.stderr[-2000:]
    assert "no input until plan 6" not in r.stderr


def test_di_rules(selftest, tmp_path):
    check(selftest, "di_rules", tmp_path)


@pytest.mark.parametrize("case, message", [
    ("caps", "IDirectInputDevice8A::GetCapabilities not implemented"),
    ("guid", "{6F1D2B61-D5A0-11CF-BFC7-444553540000}"),
    ("coop", "SetCooperativeLevel flags 0x15"),
    ("prop", "SetProperty property 4"),
    ("enum", "IDirectInput8A::EnumDevices not implemented"),
])
def test_di_loud(selftest, tmp_path, case, message):
    r = run_group(selftest, "di_loud", tmp_path, case)
    assert r.returncode != 0 and "di_loud: returned" not in r.stdout, r.stdout[-2000:]
    assert message in r.stderr, r.stderr[-2000:]
    assert "FATAL: d3d9: " in r.stderr, r.stderr[-2000:]


def test_di_quit_unset(selftest, tmp_path):
    # unset, nothing changes: all 8 reads answer and the group returns
    r = check(selftest, "di_quit", tmp_path)
    assert "di_quit: read 8 answered (0 failed so far)" in r.stdout, r.stdout[-2000:]
    assert "quit after" not in r.stderr, r.stderr[-2000:]


@pytest.mark.parametrize("how, n", [("data", 3), ("mixed", 3), ("mixed", 2), ("data", 1)])
def test_di_quit(selftest, tmp_path, how, n):
    # the nth read (GetDeviceData and GetDeviceState counted together) exits 0 before answering
    r = run_group(selftest, "di_quit", tmp_path, how, quit_after=str(n))
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert f"[bw2] quit after {n} input reads (BW2_QUIT_AFTER_READS)" in r.stderr, r.stderr[-2000:]
    assert "FAIL" not in r.stdout, r.stdout[-2000:]
    for i in range(1, n):
        assert f"di_quit: read {i} answered (0 failed so far)" in r.stdout, r.stdout[-2000:]
    assert f"di_quit: read {n} asked" in r.stdout, r.stdout[-2000:]
    assert f"di_quit: read {n} answered" not in r.stdout, r.stdout[-2000:]
    assert "di_quit: returned" not in r.stdout, r.stdout[-2000:]


@pytest.mark.parametrize("value", ["x", "0", "-2", "3x", ""])
def test_di_quit_bad_value(selftest, tmp_path, value):
    # a value that is not a positive integer stops the run at the first read, naming it
    r = run_group(selftest, "di_quit", tmp_path, "data", quit_after=value)
    assert r.returncode != 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "di_quit: read 1 asked" in r.stdout and "di_quit: read 1 answered" not in r.stdout, r.stdout[-2000:]
    assert f"BW2_QUIT_AFTER_READS={value!r} is not a positive integer" in r.stderr, r.stderr[-2000:]
    assert "FATAL: d3d9: " in r.stderr, r.stderr[-2000:]



# ---- plan 6 Task 1: the host input queue, Windows messages and IMM32 ----

def test_queue(selftest, tmp_path):
    check(selftest, "queue", tmp_path)


def test_mouse_msgs(selftest, tmp_path):
    check(selftest, "mouse_msgs", tmp_path)


def test_wheel_msgs(selftest, tmp_path):
    check(selftest, "wheel_msgs", tmp_path)


def test_key_msgs(selftest, tmp_path):
    check(selftest, "key_msgs", tmp_path)


def test_activate(selftest, tmp_path):
    check(selftest, "activate", tmp_path)


def test_close(selftest, tmp_path):
    check(selftest, "close", tmp_path)


def test_drain_points(selftest, tmp_path):
    check(selftest, "drain_points", tmp_path)


def test_imm(selftest, tmp_path):
    check(selftest, "imm", tmp_path)


# ---- plan 6 Task 2: the DirectInput mouse on real input ----

@pytest.mark.parametrize("group", ["di_format", "di_data", "di_state", "di_overflow", "focus_loss", "di_exclusive"])
def test_di_input(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


# ---- plan 6 Task 3: Mac events in the game's terms ----

@pytest.mark.parametrize("group", ["keymap", "geom_letterbox", "deltas", "scroll_precise", "option_middle", "cmd_keys",
                                   "caps", "mouse_events", "translate_reset"])
def test_translate(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


# ---- plan 6 Task 4: the game's window on the Mac ----

def test_window_size(selftest, tmp_path):
    check(selftest, "window_size", tmp_path)


# ---- plan 6 Task 5: the cursor ----

@pytest.mark.parametrize("group", ["cursor_policy", "mode_switch", "cursor_warp", "clip", "cursor_image"])
def test_cursor(selftest, tmp_path, group):
    check(selftest, group, tmp_path)
