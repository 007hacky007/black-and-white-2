"""Plan 4: the D3D9 layer on Metal, driven as lifted code drives it (runtime/bw2/d3d9/d3d9_selftest.c).

Each test runs one group of the selftest headless and expects "<group>: 0 failed"."""
import os
import subprocess
import zlib

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, ffmpeg_cflags, engine_libs,
                       engine_sources, sdk_path)


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("d3d9") / "d3d9_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "d3d9" / "d3d9_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *ffmpeg_cflags(), *map(str, srcs),
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


# ---- Task 3: COM objects, IDirect3D9, DirectDraw 7 ----

def test_adapter(selftest, tmp_path):
    check(selftest, "adapter", tmp_path)


def test_ddraw(selftest, tmp_path):
    check(selftest, "ddraw", tmp_path)


def test_many_objects(selftest, tmp_path):
    check(selftest, "objects", tmp_path)


def test_unimplemented_method_is_loud(selftest, tmp_path):
    r = run_group(selftest, "unimplemented", tmp_path)
    assert r.returncode != 0, r.stdout
    assert "IDirect3DDevice9::DrawRectPatch not implemented" in r.stderr, r.stderr[-2000:]


# ---- Task 4: device, swap chain, Present ----

@pytest.mark.parametrize("group", ["clear", "persist", "present_scaled", "reset_rules", "defaults", "fpu",
                                   "depth_objects"])
def test_device(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


# ---- Task 5: textures, surfaces, buffers ----

@pytest.mark.parametrize("group", ["lock_roundtrip", "pitches", "dxt_small_mips", "zeroed", "release_while_bound",
                                   "child_refs", "formats_check"])
def test_resources(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


# ---- Task 6: draws and the fixed-function pipeline ----

@pytest.mark.parametrize("group", ["tri_colour", "half_pixel", "transforms", "primitive_types", "dynamic_discard",
                                   "depth_objects_draw", "stages", "blend_depth_stencil", "samplers",
                                   "lighting", "fog_alpha", "cache", "autogen_rt", "vdecl_dupes",
                                   "draw_count"])
def test_draws(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


# ---- Task 7: state blocks, queries, the rest ----

@pytest.mark.parametrize("group", ["stateblock", "query_event", "query_occlusion", "lights_grow", "swapchain2"])
def test_device_rest(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


def test_cache_persists_across_runs(selftest, tmp_path):
    # each run adds its pipelines to the cache on disk; a later run finds them all
    for group in ("persist_a", "query_occlusion", "fog_alpha", "persist_b", "persist_check"):
        r = run_group(selftest, group, tmp_path)
        assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
        assert "not saved" not in r.stderr, r.stderr[-4000:]


# ---- Plan 7 Task 5: the frame log ----

def test_frame_log(selftest, tmp_path, monkeypatch):
    # with BW2_FRAME_LOG=1 each Present reads the back buffer back and prints one line of numbers
    monkeypatch.setenv("BW2_FRAME_LOG", "1")
    r = run_group(selftest, "frame_log", tmp_path)
    assert r.returncode == 0 and "frame_log: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    lines = [ln for ln in r.stderr.splitlines() if ln.startswith("[bw2] frame ")]
    assert lines == ["[bw2] frame 1: 64x64, mean 16,32,48, nonblack 4096 of 4096",
                     "[bw2] frame 2: 64x64, mean 16,32,48, nonblack 4096 of 4096",
                     "[bw2] frame 3: 64x64, mean 7,4,2, nonblack 128 of 4096",
                     "[bw2] frame 4: 64x64, mean 0,0,0, nonblack 0 of 4096",
                     "[bw2] frame 5: 32x16, mean 0,0,255, nonblack 512 of 512"], r.stderr[-4000:]
    # without it (or with another value) Present reads nothing back and prints nothing
    for value in (None, "0"):
        if value is None:
            monkeypatch.delenv("BW2_FRAME_LOG")
        else:
            monkeypatch.setenv("BW2_FRAME_LOG", value)
        r = run_group(selftest, "frame_log", tmp_path)
        assert r.returncode == 0 and "frame_log: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
        assert "[bw2] frame " not in r.stderr, r.stderr[-4000:]


# ---- Plan 6 Task 4: the current display mode ----

def test_display_mode(selftest, tmp_path):
    # the desktop before any device, the full-screen device's back buffer while it lives (also for
    # GetSystemMetrics and a window that covered the screen), the desktop again after a windowed Reset
    # or the last Release; the mode list stays the desktop's throughout
    check(selftest, "display_mode", tmp_path)


# ---- Plan 10 Task 7: uploads during a long load, and the frame dump ----

def test_mtl_upload_flood(selftest, tmp_path):
    # Task 7 Step 1's mechanism: small boxes into a big texture, each staging only what it covers, with no
    # Present; no command buffer fails and every box reads its colour back from the GPU
    r = run_group(selftest, "mtl_upload_flood", tmp_path)
    assert r.returncode == 0 and "mtl_upload_flood: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    assert "[bw2] metal: command buffer failed" not in r.stderr, r.stderr[-4000:]


def test_mtl_upload_bound(selftest, tmp_path):
    # Ruling 13: the open command buffer is sent once its staging passes 64 MB
    r = run_group(selftest, "mtl_upload_bound", tmp_path)
    assert r.returncode == 0 and "mtl_upload_bound: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    assert "[bw2] metal: command buffer failed" not in r.stderr, r.stderr[-4000:]


def read_png(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGB PNG, not interlaced (what the frame dump writes)."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, ihdr, idat = 8, None, b""
    while pos < len(data):
        n, kind = int.from_bytes(data[pos:pos + 4], "big"), data[pos + 4:pos + 8]
        body, pos = data[pos + 8:pos + 8 + n], pos + 12 + n
        if kind == b"IHDR":
            ihdr = body
        elif kind == b"IDAT":
            idat += body
    w, h = int.from_bytes(ihdr[0:4], "big"), int.from_bytes(ihdr[4:8], "big")
    assert (ihdr[8], ihdr[9], ihdr[12]) == (8, 2, 0), f"depth, colour type, interlace {tuple(ihdr[8:13])}"
    raw, stride, rows, prev = zlib.decompress(idat), w * 3, [], bytearray(w * 3)
    for y in range(h):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a, b, c = line[i - 3] if i >= 3 else 0, prev[i], prev[i - 3] if i >= 3 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            else:
                assert kind == 0, f"filter {kind}"
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def pixel(rows, x, y):
    return tuple(rows[y][3 * x:3 * x + 3])


def dump_lines(stderr):
    return [ln for ln in stderr.splitlines() if ln.startswith("[bw2] frame dump")]


def test_frame_dump(selftest, tmp_path, monkeypatch):
    # every 2nd Present writes frame-<n>.png (the back buffer, RGB) into a folder it makes, and says so
    out = tmp_path / "dump" / "frames"
    monkeypatch.setenv("BW2_FRAME_DUMP", f"{out}:2")
    r = run_group(selftest, "frame_dump", tmp_path)
    assert r.returncode == 0 and "frame_dump: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    assert dump_lines(r.stderr) == ["[bw2] frame dump: 2", "[bw2] frame dump: 4"], r.stderr[-4000:]
    assert sorted(p.name for p in out.iterdir()) == ["frame-2.png", "frame-4.png"]
    w, h, rows = read_png(out / "frame-2.png")
    assert (w, h) == (64, 48)
    assert pixel(rows, 32, 24) == (0, 255, 0)
    assert pixel(rows, 0, 0) == pixel(rows, 7, 7) == (255, 0, 0)        # the red corner: top left, RGB order
    assert pixel(rows, 8, 8) == pixel(rows, 63, 47) == (0, 255, 0)
    w, h, rows = read_png(out / "frame-4.png")
    assert (w, h) == (64, 48)
    assert pixel(rows, 32, 24) == pixel(rows, 63, 47) == (255, 255, 255)
    assert pixel(rows, 0, 0) == (0, 0, 255)
    # a relative folder is taken from the current folder
    monkeypatch.setenv("BW2_FRAME_DUMP", "rel/frames:4")
    r = run_group(selftest, "frame_dump", tmp_path)
    assert r.returncode == 0 and dump_lines(r.stderr) == ["[bw2] frame dump: 4"], r.stdout[-4000:] + r.stderr[-4000:]
    assert sorted(p.name for p in (tmp_path / "rel" / "frames").iterdir()) == ["frame-4.png"]


def test_frame_dump_default_every(selftest, tmp_path, monkeypatch):
    # without ":<every>" every 100th Present is written
    out = tmp_path / "dump"
    monkeypatch.setenv("BW2_FRAME_DUMP", str(out))
    r = run_group(selftest, "frame_dump", tmp_path, 100)
    assert r.returncode == 0 and "frame_dump: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    assert dump_lines(r.stderr) == ["[bw2] frame dump: 100"], r.stderr[-4000:]
    assert sorted(p.name for p in out.iterdir()) == ["frame-100.png"]
    w, h, rows = read_png(out / "frame-100.png")
    assert (w, h) == (64, 48) and pixel(rows, 32, 24) == (255, 255, 255)


def refused(selftest, tmp_path, value):
    """Run four Presents with BW2_FRAME_DUMP=value: the first Present stops; return the FATAL line."""
    env_run = dict(os.environ, BW2_FRAME_DUMP=value)
    r = subprocess.run([str(selftest), "frame_dump"], capture_output=True, text=True, timeout=120, cwd=tmp_path,
                       env=dict(env_run, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support")))
    assert r.returncode != 0, r.stdout[-2000:]
    assert "frame_dump: present 1" in r.stdout and "present 2" not in r.stdout, r.stdout[-2000:]
    fatal = [ln for ln in r.stderr.splitlines() if ln.startswith("[bw2] FATAL: d3d9: frame dump: ")]
    assert len(fatal) == 1, r.stderr[-2000:]
    assert not dump_lines(r.stderr), r.stderr[-2000:]
    return fatal[0]


@pytest.mark.parametrize("git", ["dir", "file"])
def test_frame_dump_refuses_git_tree(selftest, tmp_path, git):
    # frames are game material: a folder inside a git work tree (a .git folder, or a worktree's .git file, in it
    # or in any folder above it) stops at the first Present, naming both, and nothing is made there
    repo = tmp_path / "repo"
    (repo / "x" / "y").mkdir(parents=True)
    (repo / ".git").mkdir() if git == "dir" else (repo / ".git").write_text("gitdir: elsewhere\n")
    out = repo / "x" / "y" / "a" / "b"
    line = refused(selftest, tmp_path, f"{out}:2")
    assert str(out) in line and f"{repo} holds .git" in line, line
    assert not (repo / "x" / "y" / "a").exists()
    # the folder itself holding .git counts too, and so does a way in through a link or ".."
    line = refused(selftest, tmp_path, str(repo))
    assert f"{repo} holds .git" in line, line
    (tmp_path / "link").symlink_to(repo / "x")
    line = refused(selftest, tmp_path, f"{tmp_path / 'link' / 'new'}:2")
    assert f"{repo} holds .git" in line, line
    line = refused(selftest, tmp_path, f"{tmp_path / 'outside' / '..' / 'repo' / 'new'}:2")
    assert f"{repo} holds .git" in line, line
    assert not (repo / "x" / "new").exists() and not (repo / "new").exists() and not (tmp_path / "outside").exists()


def test_frame_dump_refuses_bad_folder_or_count(selftest, tmp_path):
    (tmp_path / "file").write_text("x")
    line = refused(selftest, tmp_path, f"{tmp_path / 'file' / 'sub'}:2")
    assert f"cannot make {tmp_path / 'file' / 'sub'}" in line, line
    for bad in ("0", "x", "", "-1", "99999999999"):
        line = refused(selftest, tmp_path, f"{tmp_path / 'dump'}:{bad}")
        assert f"BW2_FRAME_DUMP={tmp_path / 'dump'}:{bad}" in line, line
    assert not (tmp_path / "dump").exists()
    line = refused(selftest, tmp_path, "")
    assert "BW2_FRAME_DUMP=" in line, line
