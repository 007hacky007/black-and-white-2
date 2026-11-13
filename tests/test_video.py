"""Plan 10 Task 5: the Bink decoder core on FFmpeg (runtime/bw2/video/video_selftest.c).

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
