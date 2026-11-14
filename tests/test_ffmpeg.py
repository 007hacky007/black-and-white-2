"""Plan 13 Task 3: the LGPL-only FFmpeg the app ships, built from the pinned 9.0.2 tarball with only
the Bink demuxer and decoders (plan 10 Ruling 2). No FFmpeg source enters the repo."""
import ctypes
import json
import os
import re
import subprocess
import time
from pathlib import Path

import pytest

from bw2 import build, ffmpeg
from bw2.build import BuildError

PREFIX = Path("/fake/out/tools/ffmpeg-9.0.2")
CLANG = Path("/fake/Toolchains/usr/bin/clang")
SDK = Path("/fake/SDKs/MacOSX.sdk")
FIXED_ENV_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
SYSTEM_FRAMEWORKS = {f"/System/Library/Frameworks/{n}.framework/Versions/A/{n}"
                     for n in ("CoreFoundation", "CoreVideo", "CoreMedia")}


def test_configure_flags_lgpl():
    flags = ffmpeg.configure_flags(PREFIX, CLANG, SDK)
    assert flags == [
        f"--prefix={PREFIX}", "--install-name-dir=@rpath", "--enable-shared", "--disable-static",
        "--disable-programs", "--disable-doc", "--disable-everything", "--disable-autodetect",
        "--disable-network", "--disable-avdevice", "--disable-avfilter", "--disable-swscale",
        "--disable-swresample", "--enable-demuxer=bink", "--enable-decoder=bink,binkaudio_dct,binkaudio_rdft",
        "--arch=aarch64", "--target-os=darwin", f"--cc={CLANG}",
        f"--extra-cflags=-mcpu=apple-m1 -mmacosx-version-min=13.0 -isysroot {SDK}",
        f"--extra-ldflags=-mmacosx-version-min=13.0 -isysroot {SDK}",
    ]
    for f in flags:
        assert not any(w in f.lower() for w in ("gpl", "nonfree", "version3")), f
    spaced = Path("/Volumes/Some Disk/MacOSX.sdk")
    with pytest.raises(BuildError, match=re.escape(str(spaced))):
        ffmpeg.configure_flags(PREFIX, CLANG, spaced)


def test_ffmpeg_dylibs_by_install_name(fake_ffmpeg_prefix):
    prefix = fake_ffmpeg_prefix()
    got = ffmpeg.ffmpeg_dylibs(prefix)
    assert [n for n, _ in got] == list(ffmpeg.FFMPEG_DYLIBS)
    for name, real in got:
        assert not real.is_symlink() and real.is_file(), real
        assert real == (prefix / "lib" / (name[:-len(".dylib")] + ".1.2.dylib")).resolve()
    # an absolute install name (Homebrew's form) is refused, naming the library
    bad = fake_ffmpeg_prefix(install_name=lambda n: f"/opt/homebrew/lib/{n}")
    with pytest.raises(BuildError, match=r"libavformat\.63\.dylib.*/opt/homebrew/lib/libavformat\.63\.dylib"):
        ffmpeg.ffmpeg_dylibs(bad)
    # an @rpath name that is not the library's own
    swapped = fake_ffmpeg_prefix(install_name=lambda n: "@rpath/libavutil.61.dylib")
    with pytest.raises(BuildError, match=r"libavformat\.63\.dylib"):
        ffmpeg.ffmpeg_dylibs(swapped)
    # a missing library
    (prefix / "lib" / "libavutil.61.dylib").unlink()
    with pytest.raises(BuildError, match=r"libavutil\.61\.dylib"):
        ffmpeg.ffmpeg_dylibs(prefix)


def _fake_build(monkeypatch, tmp_path, fail_at=None):
    """build_ffmpeg with fetch, the tools and subprocess.run faked; returns the recorded calls."""
    calls = []
    monkeypatch.setattr(build, "check_tools", lambda: CLANG)
    monkeypatch.setattr(build, "sdk_path", lambda: SDK)
    monkeypatch.setattr(ffmpeg, "developer_dir", lambda: tmp_path / "Developer")
    (tmp_path / "Developer" / "usr" / "bin").mkdir(parents=True)
    (tmp_path / "Developer" / "usr" / "bin" / "make").write_text("")

    def fake_fetch(url, dest, expected):
        Path(dest).parent.mkdir(parents=True, exist_ok=True)
        Path(dest).write_bytes(b"tarball")
        return Path(dest)

    def fake_run(argv, **kw):
        argv = [str(a) for a in argv]
        calls.append((argv, kw))
        out = kw.get("stdout")
        if out is not None and hasattr(out, "write"):
            out.write(f"ran {argv[0]}\n")
        if argv[0] == "/usr/bin/tar":
            dest = Path(argv[argv.index("-C") + 1])
            (dest / "configure").write_text("")
        step = Path(argv[0]).name + (" " + argv[1] if len(argv) > 1 and argv[1] == "install" else "")
        if fail_at and step == fail_at:
            return subprocess.CompletedProcess(argv, 1)
        if argv[1:2] == ["install"]:
            dest = Path(next(a.split("=", 1)[1] for a in argv if a.startswith("DESTDIR=")))
            lib = dest / str(tmp_path / "ffmpeg-9.0.2").lstrip("/") / "lib"
            lib.mkdir(parents=True)
            for n in ffmpeg.FFMPEG_DYLIBS:
                (lib / n).write_text("")
        return subprocess.CompletedProcess(argv, 0)

    monkeypatch.setattr(ffmpeg, "fetch", fake_fetch)
    monkeypatch.setattr(ffmpeg.subprocess, "run", fake_run)
    return calls


def test_build_ffmpeg_env(monkeypatch, tmp_path):
    monkeypatch.setenv("PATH", "/opt/homebrew/bin:/usr/bin:/bin")
    monkeypatch.setenv("CFLAGS", "-O0")
    monkeypatch.setenv("PKG_CONFIG_PATH", "/opt/homebrew/lib/pkgconfig")
    calls = _fake_build(monkeypatch, tmp_path)
    prefix = ffmpeg.build_ffmpeg(tmp_path / "ffmpeg-9.0.2", tmp_path)
    assert prefix == tmp_path / "ffmpeg-9.0.2"
    want = {"PATH": FIXED_ENV_PATH, "SDKROOT": str(SDK)}
    steps = {Path(a[0]).name + (" install" if a[1:2] == ["install"] else ""): (a, kw) for a, kw in calls}
    assert set(steps) >= {"tar", "configure", "make", "make install"}, list(steps)
    for name in ("configure", "make", "make install"):
        argv, kw = steps[name]
        assert kw.get("env") == want, (name, kw.get("env"))
    cfg, _ = steps["configure"]
    assert cfg[1:] == ffmpeg.configure_flags(prefix, CLANG, SDK)
    make, _ = steps["make"]
    assert make[0] == str(tmp_path / "Developer/usr/bin/make") and make[1] == f"-j{os.cpu_count()}"
    assert steps["tar"][0][0] == "/usr/bin/tar"
    stamp = json.loads((prefix / "BW2-FFMPEG.json").read_text())
    assert stamp["flags"] == ffmpeg.configure_flags(prefix, CLANG, SDK)
    assert stamp["tarball_sha256"] == ffmpeg.FFMPEG_RELEASE().sha256
    assert not (tmp_path / "ffmpeg-9.0.2.part").exists()
    assert not (tmp_path / "ffmpeg-9.0.2.src.part").exists()
    # a finished prefix whose stamp matches is reused without running anything
    calls.clear()
    assert ffmpeg.build_ffmpeg(tmp_path / "ffmpeg-9.0.2", tmp_path) == prefix
    assert calls == []


def test_build_ffmpeg_failure_names_the_log(monkeypatch, tmp_path):
    calls = _fake_build(monkeypatch, tmp_path, fail_at="make")
    with pytest.raises(BuildError, match=r"make.*exit 1") as e:
        ffmpeg.build_ffmpeg(tmp_path / "ffmpeg-9.0.2", tmp_path)
    assert str(tmp_path / "ffmpeg-9.0.2.log") in str(e.value)
    assert "ran " in str(e.value)                        # the log's tail is in the message
    assert not (tmp_path / "ffmpeg-9.0.2").exists()
    assert not (tmp_path / "ffmpeg-9.0.2.part").exists()
    assert not (tmp_path / "ffmpeg-9.0.2.src.part").exists()


def test_build_ffmpeg_stale_stamp_rebuilds(monkeypatch, tmp_path):
    calls = _fake_build(monkeypatch, tmp_path)
    prefix = ffmpeg.build_ffmpeg(tmp_path / "ffmpeg-9.0.2", tmp_path)
    stamp = prefix / "BW2-FFMPEG.json"
    data = json.loads(stamp.read_text())
    data["flags"] = data["flags"][:-1]
    stamp.write_text(json.dumps(data))
    (tmp_path / "ffmpeg-9.0.2.part").mkdir()             # left over from an interrupted run
    calls.clear()
    assert ffmpeg.build_ffmpeg(prefix, tmp_path) == prefix
    assert any(Path(a[0]).name == "configure" for a, _ in calls)
    assert json.loads(stamp.read_text())["flags"] == ffmpeg.configure_flags(prefix, CLANG, SDK)


def test_known_release_row():
    rel = ffmpeg.FFMPEG_RELEASE()
    assert (rel.kind, rel.name, rel.size) == ("ffmpeg-source", "ffmpeg-9.0.2.tar.xz", 12040788)
    assert rel.sha256 == "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
    assert ffmpeg.FFMPEG_URL == "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz"
    assert ffmpeg.LGPL_PREFIX == ffmpeg.TOOLS_DIR / "ffmpeg-9.0.2"


def _otool(*args):
    otool = build.check_tools().parent / "otool"
    return subprocess.run([str(otool), *map(str, args)], capture_output=True, text=True, check=True).stdout


@pytest.mark.gamefiles
def test_build_ffmpeg(record_property):
    t0 = time.time()
    prefix = ffmpeg.build_ffmpeg()
    record_property("seconds", round(time.time() - t0, 1))
    assert prefix == ffmpeg.LGPL_PREFIX
    libs = dict(ffmpeg.ffmpeg_dylibs(prefix))
    # load avutil, then avcodec, then avformat by their real files: each @rpath dependency is
    # already loaded under its install name
    util = ctypes.CDLL(str(libs["libavutil.61.dylib"]), mode=ctypes.RTLD_GLOBAL)
    codec = ctypes.CDLL(str(libs["libavcodec.63.dylib"]), mode=ctypes.RTLD_GLOBAL)
    fmt = ctypes.CDLL(str(libs["libavformat.63.dylib"]), mode=ctypes.RTLD_GLOBAL)
    for lib, fn in ((codec, "avcodec_license"), (fmt, "avformat_license"), (util, "avutil_license")):
        f = getattr(lib, fn)
        f.restype = ctypes.c_char_p
        assert f() == b"LGPL version 2.1 or later", fn
    codec.avcodec_configuration.restype = ctypes.c_char_p
    conf = codec.avcodec_configuration()
    assert b"--disable-everything" in conf and b"--enable-gpl" not in conf, conf
    codec.avcodec_find_decoder_by_name.restype = ctypes.c_void_p
    codec.avcodec_find_decoder_by_name.argtypes = [ctypes.c_char_p]
    # configure's component is "bink"; the decoder registers itself as "binkvideo"
    assert codec.avcodec_find_decoder_by_name(b"binkvideo")
    assert codec.avcodec_find_decoder_by_name(b"binkaudio_dct")
    assert codec.avcodec_find_decoder_by_name(b"binkaudio_rdft")
    assert not codec.avcodec_find_decoder_by_name(b"h264")
    fmt.av_find_input_format.restype = ctypes.c_void_p
    fmt.av_find_input_format.argtypes = [ctypes.c_char_p]
    assert fmt.av_find_input_format(b"bink") and not fmt.av_find_input_format(b"mov")
    for name, real in libs.items():
        record_property(name, real.stat().st_size)
        assert _otool("-D", real).splitlines()[1] == f"@rpath/{name}"
        deps = [l.strip().split(" ")[0] for l in _otool("-L", real).splitlines()[1:] if l.strip()]
        # configure links these three system frameworks unconditionally (check_apple_framework);
        # nothing else outside the bundle and /usr/lib, and no VideoToolbox or AudioToolbox
        others = [d for d in deps if not d.startswith(("@rpath/", "/usr/lib/"))]
        assert deps and set(others) <= SYSTEM_FRAMEWORKS, (name, deps)
        load = _otool("-l", real)
        assert re.search(r"cmd LC_BUILD_VERSION\n.*\n\s+platform 1\n\s+minos 13\.0\n", load), name
        assert "LC_RPATH" not in load, name


@pytest.mark.gamefiles
def test_build_ffmpeg_reused():
    prefix = ffmpeg.build_ffmpeg()
    before = {p: p.lstat().st_mtime_ns for p in prefix.rglob("*")}
    t0 = time.time()
    assert ffmpeg.build_ffmpeg() == prefix
    assert time.time() - t0 < 2
    assert {p: p.lstat().st_mtime_ns for p in prefix.rglob("*")} == before
