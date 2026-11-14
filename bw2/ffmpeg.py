"""The LGPL-only FFmpeg the app ships (spec 9; plan 10 Ruling 2; plan 13 Task 3).

Built once per workspace from the pinned 9.0.2 release tarball (never in the repo) with only the
Bink demuxer and the three Bink decoders, as shared libraries with @rpath install names, so the
app can carry them in Contents/Frameworks. Development keeps Homebrew's build (bw2.build).

The build runs with a fixed environment (PATH=/usr/bin:/bin:/usr/sbin:/sbin, SDKROOT, the
absolute clang): nothing from the caller's environment or from Homebrew reaches it. A finished
prefix holds BW2-FFMPEG.json (the configure flags and the tarball's SHA-256) and is reused while
both match; anything half-built has a .part name.
"""
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

from . import build
from .build import BuildError, ToolsMissing
from .download import fetch
from .releases import Release, load_releases
from .tools import TOOLS_DIR

FFMPEG_VERSION = "9.0.2"
FFMPEG_URL = f"https://ffmpeg.org/releases/ffmpeg-{FFMPEG_VERSION}.tar.xz"
TARBALL = f"ffmpeg-{FFMPEG_VERSION}.tar.xz"
LGPL_PREFIX = TOOLS_DIR / f"ffmpeg-{FFMPEG_VERSION}"
STAMP = "BW2-FFMPEG.json"
FIXED_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
# Named by their install names; in FFmpeg's layout each is a symlink to e.g. libavformat.63.1.102.dylib.
FFMPEG_DYLIBS = ("libavformat.63.dylib", "libavcodec.63.dylib", "libavutil.61.dylib")


def FFMPEG_RELEASE() -> Release:
    """The pinned tarball's row in known_releases.tsv."""
    rows = [r for r in load_releases() if r.kind == "ffmpeg-source" and r.name == TARBALL]
    if len(rows) != 1:
        raise BuildError(f"known_releases.tsv must hold exactly one ffmpeg-source row for {TARBALL}")
    return rows[0]


def developer_dir() -> Path:
    """The active developer directory (xcode-select -p); never runs the /usr/bin shims."""
    r = subprocess.run(["/usr/bin/xcode-select", "-p"], capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        raise ToolsMissing("Install Xcode Command Line Tools: xcode-select --install")
    return Path(r.stdout.strip())


def configure_flags(prefix: Path, clang: Path, sdk: Path) -> list[str]:
    """FFmpeg's configure arguments, one argv element each. configure splits the extra flags on
    spaces, so a path with whitespace in them cannot work and is refused by name."""
    if any(c.isspace() for c in str(sdk)):
        raise BuildError(f"the SDK path {sdk} contains whitespace; FFmpeg's configure splits "
                         "--extra-cflags on spaces, so it cannot be used")
    return [
        f"--prefix={prefix}", "--install-name-dir=@rpath", "--enable-shared", "--disable-static",
        "--disable-programs", "--disable-doc", "--disable-everything", "--disable-autodetect",
        "--disable-network", "--disable-avdevice", "--disable-avfilter", "--disable-swscale",
        "--disable-swresample", "--enable-demuxer=bink", "--enable-decoder=bink,binkaudio_dct,binkaudio_rdft",
        "--arch=aarch64", "--target-os=darwin", f"--cc={clang}",
        f"--extra-cflags=-mcpu=apple-m1 -mmacosx-version-min=13.0 -isysroot {sdk}",
        f"--extra-ldflags=-mmacosx-version-min=13.0 -isysroot {sdk}",
    ]


def _stamp_ok(prefix: Path, flags: list[str], sha256: str) -> bool:
    try:
        data = json.loads((prefix / STAMP).read_text())
    except (OSError, ValueError):
        return False
    return data.get("flags") == flags and data.get("tarball_sha256") == sha256


def _tail_of(log: Path, n: int = 40) -> str:
    try:
        return build._tail(log.read_text(errors="replace"), n)
    except OSError:
        return ""


def build_ffmpeg(prefix: Path = LGPL_PREFIX, download_dir: Path = TOOLS_DIR) -> Path:
    """prefix, holding the LGPL-only FFmpeg; built from the pinned tarball unless already there."""
    prefix, download_dir = Path(prefix).absolute(), Path(download_dir)
    clang, sdk = build.check_tools(), build.sdk_path()
    flags = configure_flags(prefix, clang, sdk)
    release = FFMPEG_RELEASE()
    if _stamp_ok(prefix, flags, release.sha256):
        return prefix
    make = developer_dir() / "usr" / "bin" / "make"
    if not make.is_file():
        raise ToolsMissing(f"{make} is missing; reinstall Xcode Command Line Tools: xcode-select --install")
    src = prefix.with_name(prefix.name + ".src.part")
    part = prefix.with_name(prefix.name + ".part")
    log = prefix.with_name(prefix.name + ".log")
    for stale in (src, part):
        shutil.rmtree(stale, ignore_errors=True)
    tarball = fetch(FFMPEG_URL, download_dir / TARBALL, release)
    env = {"PATH": FIXED_PATH, "SDKROOT": str(sdk)}
    steps = [
        ("unpack", ["/usr/bin/tar", "-xf", str(tarball), "-C", str(src), "--strip-components", "1"], env),
        ("configure", ["./configure", *flags], env),
        ("make", [str(make), f"-j{os.cpu_count() or 4}"], env),
        ("make install", [str(make), "install", f"DESTDIR={part}"], env),
    ]
    try:
        src.mkdir(parents=True)
        part.mkdir()
        with open(log, "w") as f:
            for name, argv, step_env in steps:
                f.write(f"== {name}: {' '.join(argv)}\n")
                f.flush()
                t0 = time.time()
                try:
                    r = subprocess.run(argv, cwd=src, env=step_env, stdout=f, stderr=subprocess.STDOUT)
                except OSError as e:
                    raise BuildError(f"FFmpeg {name} could not start ({e}); full log {log}") from e
                f.write(f"== {name}: exit {r.returncode} in {time.time() - t0:.1f} s\n")
                f.flush()
                if r.returncode != 0:
                    raise BuildError(f"FFmpeg {name} failed (exit {r.returncode}); full log {log}:\n"
                                     + _tail_of(log))
        # make install put the tree at DESTDIR + prefix
        installed = part / prefix.relative_to(prefix.anchor)
        missing = [n for n in FFMPEG_DYLIBS if not (installed / "lib" / n).exists()]
        if missing:
            raise BuildError(f"FFmpeg make install left no {', '.join(missing)} under {installed / 'lib'};"
                             f" full log {log}:\n" + _tail_of(log))
        (installed / STAMP).write_text(json.dumps({"flags": flags, "tarball_sha256": release.sha256},
                                                  indent=1) + "\n")
        shutil.rmtree(prefix, ignore_errors=True)
        installed.rename(prefix)
    finally:
        shutil.rmtree(part, ignore_errors=True)
        shutil.rmtree(src, ignore_errors=True)
    return prefix


def ffmpeg_dylibs(prefix: Path) -> list[tuple[str, Path]]:
    """(install-name basename, real file) for each of FFMPEG_DYLIBS under prefix/lib.

    A copy must be named by the first element: dyld looks for @rpath/<that name>."""
    out = []
    for name in FFMPEG_DYLIBS:
        path = Path(prefix) / "lib" / name
        if not path.exists():
            raise BuildError(f"FFmpeg library {name} missing under {Path(prefix) / 'lib'}")
        real = path.resolve()
        got = build.install_name(real)
        if got != f"@rpath/{name}":
            raise BuildError(f"FFmpeg library {name} ({real}) has install name {got}, "
                             f"not @rpath/{name}")
        out.append((name, real))
    return out
