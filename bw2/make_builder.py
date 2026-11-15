"""Make `BW2 Builder.app` (plan 13 Task 8): the Swift Builder with the pipeline's sources inside it.

    python -m bw2.make_builder [--out out]

The bundle:

    Contents/MacOS/BW2Builder           `swift build -c release --package-path builder`
    Contents/Resources/source/          bw2.workspace.copy_sources: the git-tracked sources and SOURCES.txt
    Contents/Resources/AppIcon.icns     original Builder icon, converted from its committed PNG
    Contents/Info.plist                 local.bw2-port.builder; both versions from VERSION

It is built as `BW2 Builder.app.part`, quarantine removed, ad-hoc signed, verified with
`codesign --verify --deep --strict`, and only then renamed into place (an old one is moved aside first and
deleted after). swift is found under `xcode-select -p` by file, never through the /usr/bin shim. Only the
developer who makes the Builder needs swift; the Builder itself needs only the Command Line Tools.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

from .app import version
from .appicon import make_icns
from .paths import REPO
from .workspace import copy_sources

APP = "BW2 Builder.app"
BUNDLE_ID = "local.bw2-port.builder"
EXECUTABLE = "BW2Builder"
PACKAGE = REPO / "builder"
CODESIGN = "/usr/bin/codesign"
XATTR = "/usr/bin/xattr"


class MakeError(Exception):
    pass


def developer_dir() -> Path | None:
    """The active developer directory (xcode-select -p), or None."""
    try:
        r = subprocess.run(["/usr/bin/xcode-select", "-p"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    d = r.stdout.strip()
    return Path(d) if r.returncode == 0 and d else None


def find_swift() -> Path | None:
    """swift under the developer directory (the Command Line Tools' usr/bin, or Xcode's default toolchain)."""
    dev = developer_dir()
    if dev is None:
        return None
    for c in (dev / "usr" / "bin" / "swift", dev / "Toolchains" / "XcodeDefault.xctoolchain" / "usr" / "bin" / "swift"):
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def info_plist(builder_version: str) -> dict:
    return {
        "CFBundleExecutable": EXECUTABLE,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleName": "BW2 Builder",
        "CFBundleDisplayName": "BW2 Builder",
        "CFBundleIconFile": "AppIcon.icns",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": builder_version,
        "CFBundleVersion": builder_version,
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": "13.0",
        "LSArchitecturePriority": ["arm64"],
        "NSHighResolutionCapable": True,
        "BW2BuilderVersion": builder_version,
    }


def _tail(text: str, n: int = 40) -> str:
    return "\n".join(text.splitlines()[-n:])


def _run(argv: list, what: str) -> str:
    try:
        r = subprocess.run([str(a) for a in argv], capture_output=True, text=True)
    except OSError as e:
        raise MakeError(f"{what}: cannot start {argv[0]}: {e}") from e
    if r.returncode != 0:
        raise MakeError(f"{what} failed (exit {r.returncode}):\n{_tail(r.stdout + r.stderr)}")
    return r.stdout


def swift_build(swift: Path, package: Path = PACKAGE) -> Path:
    """The release build of the Builder's executable; returns its path."""
    base = [swift, "build", "-c", "release", "--package-path", package]
    _run(base, "swift build")
    exe = Path(_run(base + ["--show-bin-path"], "swift build --show-bin-path").strip()) / EXECUTABLE
    if not exe.is_file():
        raise MakeError(f"swift build made no {exe}")
    return exe


def _remove(p: Path) -> None:
    if p.is_dir() and not p.is_symlink():
        shutil.rmtree(p)
    elif os.path.lexists(p):
        p.unlink()


def make_builder(out: Path = REPO / "out", root: Path = REPO) -> Path:
    """Builds, signs and verifies <out>/BW2 Builder.app; returns its path."""
    swift = find_swift()
    if swift is None:
        raise MakeError(f"no swift under xcode-select -p ({developer_dir() or 'nothing selected'}): "
                        "making BW2 Builder.app needs Xcode or the Command Line Tools")
    builder_version = version()
    exe = swift_build(swift)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    app = out / APP
    part = out / (APP + ".part")
    _remove(part)
    try:
        contents = part / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        shutil.copy2(exe, contents / "MacOS" / EXECUTABLE)
        copy_sources(contents / "Resources" / "source", root=root)
        try:
            make_icns(PACKAGE / "Resources" / "AppIcon-1024.png", contents / "Resources" / "AppIcon.icns")
        except subprocess.CalledProcessError as e:
            detail = (e.stderr or b"").decode(errors="replace").strip()
            raise MakeError(f"Builder icon failed: {detail or e}") from e
        (contents / "Info.plist").write_bytes(plistlib.dumps(info_plist(builder_version)))
        _run([XATTR, "-dr", "com.apple.quarantine", part], "xattr")
        _run([CODESIGN, "--force", "--sign", "-", part], "codesign")
        _run([CODESIGN, "--verify", "--deep", "--strict", part], "codesign --verify")
        old = None
        if os.path.lexists(app):
            old = out / f".trash-{uuid.uuid4()}"
            app.rename(old)
        try:
            part.rename(app)
        except OSError:
            if old is not None:
                old.rename(app)
            raise
        if old is not None:
            _remove(old)
    except BaseException:
        _remove(part)
        raise
    return app


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.make_builder", description="Make BW2 Builder.app.")
    ap.add_argument("--out", type=Path, default=REPO / "out", help="the folder that gets the app (default out)")
    a = ap.parse_args(argv)
    try:
        app = make_builder(out=a.out)
    except (MakeError, OSError) as e:
        print(f"bw2.make_builder: {e}", file=sys.stderr)
        return 1
    print(app)
    return 0


if __name__ == "__main__":
    sys.exit(main())
