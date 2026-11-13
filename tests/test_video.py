"""Plan 10 Tasks 5 and 6: the Bink decoder core on FFmpeg, and the binkw32 imports the game calls, answered
through the shims on a headless device (runtime/bw2/video/video_selftest.c).

Each test runs one group of the selftest headless and expects "<group>: 0 failed". The movies
are read from the game tree; nothing decoded is written anywhere."""
import os
import subprocess

import pytest

from bw2 import paths
from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, ffmpeg_cflags, sdk_path)

GAME = paths.GAMEFILES / "game"
EALOGO = GAME / "Data" / "Splash" / "EAlogocompressed.bik"
STUFF = GAME / "Data" / "Everything.stuff"
# offset, length; then width, height, frames, rate, rate divisor, audio tracks ("Measured" item 2)
ATTRACT = (0x30D3C440, 32540316, 720, 576, 2033, 25, 1, 0)
INTRO = (0x3B02BD00, 51782036, 640, 360, 1798, 5000000, 208333, 0)


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("video") / "video_selftest"
    src = BW2RT / "video" / "video_selftest.c"
    assert src.exists(), f"{src} is missing"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [src]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *ffmpeg_cflags(),
                        *map(str, srcs), *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def check(exe, group, tmp_path, *args, timeout=120):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    r = subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=timeout,
                       cwd=tmp_path, env=env)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, \
        f"exit {r.returncode}\n" + r.stdout[-4000:] + r.stderr[-4000:]
    return r


def test_bink_header(selftest, tmp_path):
    check(selftest, "bink_header", tmp_path)


@pytest.mark.gamefiles
@pytest.mark.parametrize("group", ["bink_ealogo", "bink_last_frame", "bink_copy_clip"])
def test_bink_loose(selftest, tmp_path, group):
    check(selftest, group, tmp_path, EALOGO)


@pytest.mark.gamefiles
@pytest.mark.parametrize("movie", [ATTRACT, INTRO], ids=["attract", "intro"])
def test_bink_archive(selftest, tmp_path, movie):
    r = check(selftest, "bink_archive", tmp_path, STUFF, hex(movie[0]), *movie[1:], timeout=300)
    assert f"bink_archive: {movie[4]} frames in " in r.stdout, r.stdout[-2000:]


@pytest.mark.gamefiles
def test_bink_two_movies(selftest, tmp_path):
    check(selftest, "bink_two_movies", tmp_path, STUFF, hex(ATTRACT[0]), ATTRACT[1], EALOGO)


# ---- Task 6: the binkw32 imports, through the shims, on a headless device ----
# Every binkw32 group takes the archive, the attract's and the intro's offset and length, and the EA logo.
def bink_args():
    return [STUFF, hex(ATTRACT[0]), ATTRACT[1], hex(INTRO[0]), INTRO[1], EALOGO]


def run_group(exe, group, tmp_path, *extra, env_extra=None, timeout=300):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"), **(env_extra or {}))
    for k in ("BW2_BINK_LOG", "BW2_QUIT_AFTER_MOVIE"):
        if k not in (env_extra or {}):
            env.pop(k, None)
    return subprocess.run([str(exe), group, *map(str, bink_args()), *map(str, extra)], capture_output=True,
                          text=True, timeout=timeout, cwd=tmp_path, env=env)


def bink_check(exe, group, tmp_path, env_extra=None):
    r = run_group(exe, group, tmp_path, env_extra=env_extra)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, \
        f"exit {r.returncode}\n" + r.stdout[-4000:] + r.stderr[-4000:]
    return r


OPEN_LINE = "[bw2] bink open: 720x576, 2033 frames at 25/1 fps, 0 audio tracks, 32540316 bytes from memory"


@pytest.mark.gamefiles
def test_binkw32_open(selftest, tmp_path):
    r = bink_check(selftest, "binkw32_open", tmp_path, {"BW2_BINK_LOG": "1"})
    assert r.stderr.splitlines().count(OPEN_LINE) == 1, r.stderr[-2000:]
    # unset, the log prints nothing
    r = bink_check(selftest, "binkw32_open", tmp_path)
    assert "[bw2] bink" not in r.stderr, r.stderr[-2000:]


@pytest.mark.gamefiles
def test_binkw32_frame(selftest, tmp_path):
    bink_check(selftest, "binkw32_frame", tmp_path)
    # the game hands _BinkDX9SurfaceType@4's answer to the copy as its flags: an unmapped format stops there
    loud(selftest, tmp_path, "copy_unmapped", "0xFFFFFFFF")


@pytest.mark.gamefiles
def test_binkw32_goto_clamp(selftest, tmp_path):
    r = bink_check(selftest, "binkw32_goto_clamp", tmp_path, {"BW2_BINK_LOG": "1"})
    lines = r.stderr.splitlines()
    assert lines.count("[bw2] bink last frame: 2033") == 1, r.stderr[-2000:]
    closes = [ln for ln in lines if ln.startswith("[bw2] bink close: ")]
    assert closes == ["[bw2] bink close: 2033 frames decoded, largest goto 2147483647"], closes


@pytest.mark.gamefiles
def test_binkw32_surface_type(selftest, tmp_path):
    bink_check(selftest, "binkw32_surface_type", tmp_path)


@pytest.mark.gamefiles
def test_binkw32_reopen(selftest, tmp_path):
    bink_check(selftest, "binkw32_reopen", tmp_path)


def loud(exe, tmp_path, case, *named, code=-6, env_extra=None):
    r = run_group(exe, "binkw32_loud", tmp_path, case, env_extra=env_extra)
    want = "[bw2] reached" if code == 4 else "[bw2] FATAL: bink: "
    assert r.returncode == code and want in r.stderr, f"{case}: exit {r.returncode}\n{r.stderr[-3000:]}"
    line = next(ln for ln in r.stderr.splitlines() if want in ln)
    for n in named:
        assert n in line, f"{case}: {n!r} not in {line!r}"
    assert "(from guest 0x12345678, " in line, line
    return line


@pytest.mark.gamefiles
@pytest.mark.parametrize("case,named", [
    ("open_flags", ["_BinkOpen@8", "0x00800000"]),
    ("goto_flags", ["_BinkGoto@12", "0x00000001"]),
    ("copy_flags", ["_BinkCopyToBuffer@28", "0x80000003"]),
    ("copy_unmapped", ["_BinkCopyToBuffer@28", "0xFFFFFFFF"]),
    ("copy_before_frame", ["_BinkCopyToBuffer@28", "_BinkDoFrame@4"]),
    ("unknown_goto", ["_BinkGoto@12", "unknown handle 0x"]),
    ("unknown_doframe", ["_BinkDoFrame@4", "unknown handle 0x"]),
    ("unknown_copy", ["_BinkCopyToBuffer@28", "unknown handle 0x"]),
    ("unknown_close", ["_BinkClose@4", "unknown handle 0x"]),
    ("not_bink", ["_BinkOpen@8", "not a Bink movie", "0x"]),
    ("null_movie", ["_BinkOpen@8", "0x00000000"]),
])
def test_binkw32_loud(selftest, tmp_path, case, named):
    loud(selftest, tmp_path, case, *named)


@pytest.mark.gamefiles
def test_binkw32_audio_track(selftest, tmp_path):
    line = loud(selftest, tmp_path, "audio_track", code=4)
    assert line.startswith("[bw2] reached Bink audio track: owned by a later plan"), line


QUIT_LINE = "[bw2] quit at movie frame 2033:3 + 2 presents (BW2_QUIT_AFTER_MOVIE)"


@pytest.mark.gamefiles
def test_binkw32_quit(selftest, tmp_path):
    # the intro's copies (1,798 frames) never arm a switch that names the attract; the attract's frames 1-2 do
    # not either; the quit comes at the second Present after the attract's frame 3
    r = run_group(selftest, "binkw32_quit", tmp_path, env_extra={"BW2_QUIT_AFTER_MOVIE": "2033:3+2"})
    assert r.returncode == 0, f"exit {r.returncode}\n{r.stdout[-2000:]}{r.stderr[-3000:]}"
    marks = [ln for ln in r.stdout.splitlines() if ln.startswith("mark ")]
    assert marks == ["mark intro present 1", "mark intro present 2", "mark attract 1 copied", "mark attract 2 present",
                     "mark attract 3 present 1"], r.stdout[-2000:]
    assert r.stderr.splitlines().count(QUIT_LINE) == 1, r.stderr[-2000:]
    assert "binkw32_quit: " not in r.stdout, r.stdout[-2000:]


@pytest.mark.gamefiles
def test_binkw32_quit_at_copy(selftest, tmp_path):
    # no presents: the copy that reaches the frame quits at once
    r = run_group(selftest, "binkw32_quit", tmp_path, env_extra={"BW2_QUIT_AFTER_MOVIE": "2033:2"})
    assert r.returncode == 0, f"exit {r.returncode}\n{r.stdout[-2000:]}{r.stderr[-3000:]}"
    marks = [ln for ln in r.stdout.splitlines() if ln.startswith("mark ")]
    assert marks == ["mark intro present 1", "mark intro present 2", "mark attract 1 copied"], r.stdout[-2000:]
    assert "[bw2] quit at movie frame 2033:2 + 0 presents (BW2_QUIT_AFTER_MOVIE)" in r.stderr.splitlines()


@pytest.mark.gamefiles
@pytest.mark.parametrize("value", ["x", "100", "2033:", ":3", "2033:3+", "0:3", "2033:0", "2033:3+x", "-1:3"])
def test_binkw32_quit_malformed(selftest, tmp_path, value):
    r = run_group(selftest, "binkw32_quit", tmp_path, env_extra={"BW2_QUIT_AFTER_MOVIE": value})
    assert r.returncode == -6, f"exit {r.returncode}\n{r.stderr[-3000:]}"
    line = next((ln for ln in r.stderr.splitlines() if "[bw2] FATAL: bink: " in ln), "")
    assert f"BW2_QUIT_AFTER_MOVIE='{value}'" in line, r.stderr[-3000:]
