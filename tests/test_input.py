"""Plan 5 Task 1: a minimal DirectInput mouse, driven as lifted code drives it (runtime/bw2/input/input_selftest.c).
Plan 6 Task 1: the host input queue, the Windows messages it makes and IMM32.
Plan 6 Task 2: the DirectInput mouse fed from the host input queue.
Plan 6 Task 3: Mac events translated into the game's terms (translate.c).
Plan 6 Task 5: the cursor policy (cursor.c) and the cursor shims.
Plan 6 Task 6: BW2_INPUT_SCRIPT (script.c), pushed at the always-counting read counter.

Each test runs one group of the selftest headless and expects "<group>: 0 failed"."""
import os
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, ffmpeg_cflags, engine_libs,
                       engine_sources, sdk_path)

FIRST_READ = "[bw2] DirectInput mouse: first read"


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("input") / "input_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "input" / "input_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *ffmpeg_cflags(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args, quit_after=None, script=None):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    env.pop("BW2_QUIT_AFTER_READS", None)
    env.pop("BW2_INPUT_SCRIPT", None)
    if quit_after is not None:
        env["BW2_QUIT_AFTER_READS"] = quit_after
    if script is not None:
        env["BW2_INPUT_SCRIPT"] = str(script)
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


# ---- plan 6 Task 6: scripted input ----

SCRIPT = """# the test's script: read, verb, fields
3 move 510 382
5 down 0
6 up 0

7 key 8 0x0E down   # Backspace
7 key 8 0x0E up
9 move -88 45
9 wheel -120
"""


def write_script(tmp_path, text, name="input.script"):
    p = tmp_path / name
    p.write_text(text)
    return p


def test_script(selftest, tmp_path):
    # BW2_QUIT_AFTER_READS unset: the read numbers come from the always-counting read counter
    r = run_group(selftest, "script", tmp_path, script=write_script(tmp_path, SCRIPT))
    assert r.returncode == 0 and "script: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    assert "script: read 9 answered (0 failed so far)" in r.stdout, r.stdout[-2000:]
    assert "quit after" not in r.stderr, r.stderr[-2000:]


def test_script_quit(selftest, tmp_path):
    # the script and BW2_QUIT_AFTER_READS share the counter: the down on read 5, the exit at read 6,
    # before that read's events
    r = run_group(selftest, "script", tmp_path, script=write_script(tmp_path, SCRIPT), quit_after="6")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "[bw2] quit after 6 input reads (BW2_QUIT_AFTER_READS)" in r.stderr, r.stderr[-2000:]
    assert "FAIL" not in r.stdout, r.stdout[-2000:]
    assert "script: read 5 answered (0 failed so far)" in r.stdout, r.stdout[-2000:]
    assert "script: read 6 asked" in r.stdout and "script: read 6 answered" not in r.stdout, r.stdout[-2000:]


@pytest.mark.parametrize("group", ["script_reads", "script_nowin"])
def test_script_direct(selftest, tmp_path, group):
    # the group writes its own script to the path it is given and calls bw2_script_at_read itself
    check(selftest, group, tmp_path, tmp_path / "direct.script")


@pytest.mark.parametrize("text, words", [
    ("2 jump 1 1\n", ["BW2_INPUT_SCRIPT line 1:", "jump"]),
    ("5 move 1 1\n4 move 1 1\n", ["BW2_INPUT_SCRIPT line 2:"]),
    ("1 wheel 100\n", ["BW2_INPUT_SCRIPT line 1:", "wheel"]),
    ("1 down 7\n", ["BW2_INPUT_SCRIPT line 1:", "button"]),
    ("# a comment\n\n1 move 1\n", ["BW2_INPUT_SCRIPT line 3:", "move"]),
    ("1 close now\n", ["BW2_INPUT_SCRIPT line 1:", "close"]),
    ("1 move 1 x\n", ["BW2_INPUT_SCRIPT line 1:", "'x'"]),
    ("1 key 8 0x0E sideways\n", ["BW2_INPUT_SCRIPT line 1:", "sideways"]),
    ("1 wheel 0\n", ["BW2_INPUT_SCRIPT line 1:", "wheel"]),
    ("0 close\n", ["BW2_INPUT_SCRIPT line 1:", "read"]),
    ("move 1 1\n", ["BW2_INPUT_SCRIPT line 1:"]),
    (None, ["no-such.script", "BW2_INPUT_SCRIPT"]),
])
def test_script_bad(selftest, tmp_path, text, words):
    # each in a child: the first read stops the run through d3d_fail, naming the line (or the file)
    path = tmp_path / "no-such.script" if text is None else write_script(tmp_path, text)
    r = run_group(selftest, "script", tmp_path, script=path)
    assert r.returncode != 0 and "script: read 1 answered" not in r.stdout, r.stdout[-2000:] + r.stderr[-2000:]
    assert "script: read 1 asked" in r.stdout, r.stdout[-2000:]
    assert "FATAL: d3d9: " in r.stderr, r.stderr[-2000:]
    for w in words:
        assert w in r.stderr, r.stderr[-2000:]


# ---- plan 11 Task 1: script blocks anchored on movies and wall time ----

AFTER_LINES = {
    "anchor": ["[bw2] script: after open 1798 at read 3"],
    "stale": ["[bw2] script: after last 1798 at read 6"],
    "empty_block": ["[bw2] script: after open 1798 at read 5", "[bw2] script: after seconds 0.5 at read 8"],
    "holds": ["[bw2] script: after seconds 0.5 at read 513", "[bw2] script: after seconds 0.5 at read 1025"],
    "holds_slow": ["[bw2] script: after seconds 0.5 at read 2", "[bw2] script: after seconds 0.5 at read 3"],
}


@pytest.mark.parametrize("case", list(AFTER_LINES))
def test_script_after(selftest, tmp_path, case):
    # each case in its own process (script.c loads its file once); the group checks at which read each
    # event is pushed and each anchor line printed, and the lines reach stderr in file order
    r = check(selftest, "script_after", tmp_path, case, tmp_path / "after.script")
    lines = [ln for ln in r.stderr.splitlines() if ln.startswith("[bw2] script:")]
    assert lines == AFTER_LINES[case], r.stderr[-2000:]


def test_script_after_quit(selftest, tmp_path):
    # 5 quit: exit 0 at read 5, before answering it
    r = run_group(selftest, "script_after", tmp_path, "quit", tmp_path / "after.script")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "[bw2] script: quit at read 5" in r.stderr, r.stderr[-2000:]
    assert "FAIL" not in r.stdout, r.stdout[-2000:]
    assert "script_after: read 4 answered (0 failed so far)" in r.stdout, r.stdout[-2000:]
    assert "script_after: read 5 asked" in r.stdout, r.stdout[-2000:]
    assert "script_after: read 5 answered" not in r.stdout, r.stdout[-2000:]
    assert "script_after: returned" not in r.stdout, r.stdout[-2000:]


@pytest.mark.parametrize("text, words", [
    ("1 down 0\nafter soon 3\n", ["BW2_INPUT_SCRIPT line 2:", "soon"]),
    ("after seconds 0\n", ["BW2_INPUT_SCRIPT line 1:", "seconds"]),
    ("after seconds -1\n", ["BW2_INPUT_SCRIPT line 1:", "-1"]),
    ("after seconds x\n", ["BW2_INPUT_SCRIPT line 1:", "'x'"]),
    ("after seconds 3601\n", ["BW2_INPUT_SCRIPT line 1:", "3601"]),
    ("after seconds 0.5 2\n", ["BW2_INPUT_SCRIPT line 1:", "after seconds"]),
    ("after open 0\n", ["BW2_INPUT_SCRIPT line 1:", "frames"]),
    ("after open\n", ["BW2_INPUT_SCRIPT line 1:", "after open"]),
    ("after last x\n", ["BW2_INPUT_SCRIPT line 1:", "'x'"]),
    ("after\n", ["BW2_INPUT_SCRIPT line 1:", "after"]),
    ("after open 1798\n3 down 0\n2 up 0\n", ["BW2_INPUT_SCRIPT line 3:", "read 2"]),
    ("after open 1798\n-1 down 0\n", ["BW2_INPUT_SCRIPT line 2:", "read"]),
    ("1 quit 1\n", ["BW2_INPUT_SCRIPT line 1:", "quit"]),
    ("2 move 1 1\nquit\n", ["BW2_INPUT_SCRIPT line 2:", "quit"]),
])
def test_script_after_bad(selftest, tmp_path, text, words):
    # as test_script_bad: the first read stops the run, naming the line
    r = run_group(selftest, "script", tmp_path, script=write_script(tmp_path, text))
    assert r.returncode != 0 and "script: read 1 answered" not in r.stdout, r.stdout[-2000:] + r.stderr[-2000:]
    assert "script: read 1 asked" in r.stdout, r.stdout[-2000:]
    assert "FATAL: d3d9: " in r.stderr, r.stderr[-2000:]
    for w in words:
        assert w in r.stderr, r.stderr[-2000:]
