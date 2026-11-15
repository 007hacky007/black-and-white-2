"""Package the engine and the game as `Black & White 2.app` (spec 7.5; plan 13 Task 5).

The bundle is laid out as the engine expects it when it runs as an app's executable (runtime/bw2/app.c):

    Contents/MacOS/bw2                  the engine
    Contents/Resources/d3dx9_25.dll     the lifted DLL (codesign refuses a DLL in MacOS)
    Contents/Resources/game/            the game tree, APFS-cloned and checked against its manifest
    Contents/Frameworks/<install name>  the LGPL FFmpeg libraries, when the engine links them by @rpath
    Contents/Info.plist

Before anything is written, the engine must link nothing but the system (/usr/lib/, /System/) and the FFmpeg
libraries of the given prefix by @rpath, with no LC_RPATH but @executable_path/../Frameworks: otherwise dyld would
stop the app before main, before its log exists, and from Finder it would just vanish. The bundle is built under a
hidden staging folder under its final .app name, its quarantine attributes removed, ad-hoc signed and verified
(codesign --verify --deep --strict), and only then moved into place, so a half-built app never looks finished and
an old app is replaced only by a good one.

    python -m bw2.app --engine E --ffmpeg P --out DIR [--app-version V] [--builder-version B]
"""
import argparse
import os
import plistlib
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

from . import appicon, gametree
from .build import APP_RPATH, BuildError, check_tools
from .ffmpeg import ffmpeg_dylibs
from .paths import GAMEFILES, REPO

APP_NAME = "Black & White 2.app"
BUNDLE_ID = "local.bw2-port.game"
ENGINE = "bw2"
DLL = "d3dx9_25.dll"
SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
CODESIGN = "/usr/bin/codesign"
XATTR = "/usr/bin/xattr"
LSREGISTER = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"


class PackageError(Exception):
    pass


def version() -> str:
    """The app's version: the one line of the repo's VERSION file."""
    v = (REPO / "VERSION").read_text().strip()
    if not v or any(c.isspace() for c in v):
        raise PackageError(f"{REPO / 'VERSION'} must hold one version, not {v!r}")
    return v


def info_plist(app_version: str, builder_version: str) -> dict:
    return {
        "CFBundleExecutable": ENGINE,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleName": "Black & White 2",
        "CFBundleDisplayName": "Black & White 2",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": app_version,
        "CFBundleVersion": app_version,
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": "13.0",
        "LSArchitecturePriority": ["arm64"],
        "LSApplicationCategoryType": "public.app-category.strategy-games",
        "NSHighResolutionCapable": True,
        "BW2BuilderVersion": builder_version,
    }


# ---- what the engine links ---------------------------------------------------------------------------------------

def _otool(*args: str) -> str:
    otool = check_tools().parent / "otool"
    r = subprocess.run([str(otool), *args], capture_output=True, text=True)
    if r.returncode != 0 or "not an object file" in r.stdout + r.stderr:
        raise PackageError(f"{args[-1]} is not a Mach-O file (otool {args[0]}: {(r.stderr or r.stdout).strip()})")
    return r.stdout


def linked_libraries(binary: Path) -> list[str]:
    """The install names a Mach-O file loads (otool -L), its own install name first for a dylib."""
    out = []
    for line in _otool("-L", str(binary)).splitlines()[1:]:
        line = line.strip()
        if line:
            out.append(re.sub(r" \(compatibility version [^)]*\)$", "", line))
    return out


def rpaths(binary: Path) -> list[str]:
    """The LC_RPATH entries of a Mach-O file (otool -l), in load-command order."""
    out, in_rpath = [], False
    for line in _otool("-l", str(binary)).splitlines():
        s = line.strip()
        if s.startswith("cmd "):
            in_rpath = s == "cmd LC_RPATH"
        elif in_rpath and s.startswith("path "):
            m = re.fullmatch(r"path (.*) \(offset \d+\)", s)
            out.append(m.group(1) if m else s[len("path "):])
            in_rpath = False
    return out


def _check_links(binary: Path, what: str, names: set[str], prefix) -> list[str]:
    """Refuses a library outside the system and the prefix's @rpath names; returns the @rpath ones."""
    via_rpath = []
    for lib in linked_libraries(binary):
        if lib.startswith(SYSTEM_PREFIXES):
            continue
        if lib.startswith("@rpath/"):
            if lib[len("@rpath/"):] not in names:
                given = f"the FFmpeg libraries of {prefix} ({', '.join(sorted(names))})" if names else \
                    "no FFmpeg prefix was given"
                raise PackageError(f"{what} {binary} links {lib}, which the app cannot carry: {given}")
            via_rpath.append(lib)
            continue
        raise PackageError(f"{what} {binary} links {lib}, outside /usr/lib/, /System/ and @rpath/; "
                           "the app may link only the system and its own Frameworks")
    return via_rpath


def check_engine(engine: Path, ffmpeg_prefix: Path | None) -> list[tuple[str, Path]]:
    """Refuses an engine the app could not start from anywhere; returns the dylibs to put in Frameworks."""
    try:
        dylibs = ffmpeg_dylibs(ffmpeg_prefix) if ffmpeg_prefix is not None else []
    except BuildError as e:
        raise PackageError(str(e)) from e
    names = {n for n, _ in dylibs}
    via_rpath = _check_links(engine, "engine", names, ffmpeg_prefix)
    found = rpaths(engine)
    for rp in found:
        if rp != APP_RPATH:
            raise PackageError(f"engine {engine} has LC_RPATH {rp}; the only one allowed is {APP_RPATH}")
    if via_rpath and not found:
        raise PackageError(f"engine {engine} links {via_rpath[0]} but has no LC_RPATH; it needs {APP_RPATH}")
    for name, real in dylibs:
        _check_links(real, f"FFmpeg library {name}", names, ffmpeg_prefix)
    return dylibs


# ---- signing -----------------------------------------------------------------------------------------------------

def _codesign(*args: str) -> None:
    r = subprocess.run([CODESIGN, *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise PackageError(f"codesign {' '.join(args)} failed (exit {r.returncode}):\n{(r.stderr + r.stdout).strip()}")


def sign_app(app: Path) -> None:
    """Ad-hoc signs each Frameworks dylib, then the bundle (no --deep)."""
    fw = Path(app) / "Contents" / "Frameworks"
    for lib in sorted(fw.iterdir()) if fw.is_dir() else []:
        _codesign("--force", "--sign", "-", str(lib))
    _codesign("--force", "--sign", "-", str(app))


def verify_app(app: Path) -> None:
    _codesign("--verify", "--deep", "--strict", str(app))


# ---- building and installing -------------------------------------------------------------------------------------

def _remove(p: Path) -> None:
    if p.is_dir() and not p.is_symlink():
        shutil.rmtree(p)
    elif os.path.lexists(p):
        p.unlink()


def _same_volume(a: Path, b: Path) -> bool:
    return os.stat(a).st_dev == os.stat(b).st_dev


def _known_app(bundle: Path) -> bool:
    contents = bundle / "Contents"
    info = contents / "Info.plist"
    if bundle.is_symlink() or contents.is_symlink() or info.is_symlink():
        return False
    try:
        value = plistlib.loads(info.read_bytes())
        return isinstance(value, dict) and value.get("CFBundleIdentifier") == BUNDLE_ID
    except (OSError, ValueError, plistlib.InvalidFileException):
        return False


def _sweep_installs(dest_dir: Path, keeping: Path | None = None) -> None:
    """Sweep owned staging folders; recover a known game backup before deleting old installs."""
    target = dest_dir / APP_NAME
    for item in sorted(dest_dir.iterdir()):
        if item == keeping:
            continue
        prefix = next((p for p in (".bw2-install-", ".trash-") if item.name.startswith(p)), None)
        if prefix is None:
            if item.name == APP_NAME + ".part":
                _remove(item)
            continue
        try:
            uuid.UUID(item.name[len(prefix):])
        except ValueError:
            continue
        try:
            if prefix == ".trash-":
                if not _known_app(item):
                    continue
                if not os.path.lexists(target):
                    os.rename(item, target)
                    continue
                if not _known_app(target):
                    continue
            _remove(item)
        except (OSError, ValueError, plistlib.InvalidFileException):
            pass


def _refresh_bundle(target: Path) -> None:
    # Registration is a Finder refresh hint; failure must not undo a completed, verified install.
    try:
        if Path(LSREGISTER).is_file():
            r = subprocess.run([LSREGISTER, "-f", str(target)], capture_output=True, text=True)
            if r.returncode:
                print(f"bw2.app: LaunchServices registration failed: {r.stderr.strip()}", file=sys.stderr)
    except OSError as e:
        print(f"bw2.app: LaunchServices registration failed: {e}", file=sys.stderr)
    try:
        target.touch()
    except OSError as e:
        print(f"bw2.app: could not refresh the bundle date: {e}", file=sys.stderr)


def install_app(built: Path, dest_dir: Path) -> Path:
    """Stage a verified bundle under its final .app name next to the destination, then replace atomically.
    Cross-volume sources are copied and retained. A failed install restores the old app and any moved source."""
    built, dest_dir = Path(built), Path(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)
    in_staging = built.parent.parent == dest_dir and built.parent.name.startswith(".bw2-install-")
    _sweep_installs(dest_dir, keeping=built.parent if in_staging else None)
    staging_dir = built.parent if in_staging else dest_dir / f".bw2-install-{uuid.uuid4()}"
    if not in_staging:
        staging_dir.mkdir()
    staged = staging_dir / APP_NAME
    target = dest_dir / APP_NAME
    moved = copied = False
    try:
        if not in_staging:
            if _same_volume(built, dest_dir):
                os.rename(built, staged)
                moved = True
            else:
                copied = True
                r = subprocess.run(["/bin/cp", "-c", "-R", str(built), str(staged)], capture_output=True, text=True)
                if r.returncode != 0:
                    raise PackageError(f"copying {built} to {staged} failed (exit {r.returncode}): {r.stderr.strip()}")
        verify_app(staged)
        old = None
        if os.path.lexists(target):
            old = dest_dir / f".trash-{uuid.uuid4()}"
            try:
                os.rename(target, old)
            except OSError as e:
                raise PackageError(f"cannot move the old app {target} aside: {e}") from e
        try:
            os.rename(staged, target)
        except OSError as e:
            if old is not None:
                try:
                    os.rename(old, target)
                except OSError as e2:
                    raise PackageError(f"cannot move {staged} to {target}: {e}; and the old app could not be put back "
                                       f"from {old}: {e2}") from e
            raise PackageError(f"cannot move {staged} to {target}: {e}") from e
        _refresh_bundle(target)
        if old is not None:
            try:
                _remove(old)
            except OSError as e:
                raise PackageError(f"installed {target}, but the old app left at {old} could not be deleted: {e}") from e
        return target
    except BaseException:
        if moved and os.path.lexists(staged) and not os.path.lexists(built):
            try:
                os.rename(staged, built)
            except OSError as e:
                raise PackageError(f"could not restore the build to {built}; it remains at {staged}: {e}") from e
        elif copied:
            _remove(staged)
        raise
    finally:
        try:
            staging_dir.rmdir()
        except OSError:
            pass


def package_app(engine: Path, d3dx_dll: Path, ffmpeg_prefix: Path | None, game: Path, dest_dir: Path,
                app_version: str, builder_version: str, manifest: Path = gametree.MANIFEST) -> Path:
    """Builds, signs and verifies <dest_dir>/<APP_NAME>; returns its path. Nothing is written to dest_dir until the
    engine, the FFmpeg libraries, the DLL and the game have been checked."""
    engine, d3dx_dll, game, dest_dir = Path(engine), Path(d3dx_dll), Path(game), Path(dest_dir)
    dylibs = check_engine(engine, Path(ffmpeg_prefix) if ffmpeg_prefix is not None else None)
    if not d3dx_dll.is_file():
        raise PackageError(f"no lifted DLL at {d3dx_dll}: build it first (python -m bw2.pipeline)")
    gametree.verify_manifest(game, Path(manifest))
    dest_dir.mkdir(parents=True, exist_ok=True)
    _sweep_installs(dest_dir)
    staging_dir = dest_dir / f".bw2-install-{uuid.uuid4()}"
    part = staging_dir / APP_NAME
    try:
        contents = part / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        shutil.copyfile(engine, contents / "MacOS" / ENGINE)
        os.chmod(contents / "MacOS" / ENGINE, 0o755)
        res = contents / "Resources"
        res.mkdir()
        shutil.copyfile(d3dx_dll, res / DLL)
        gametree.clone_tree(game, res / "game")
        if dylibs:
            fw = contents / "Frameworks"
            fw.mkdir()
            for name, real in dylibs:          # named as dyld asks for it, never by the file it resolves to
                shutil.copyfile(real, fw / name)
                os.chmod(fw / name, 0o755)
        info = info_plist(app_version, builder_version)
        if appicon.make_game_icon(game / "white.exe", res / "AppIcon.icns"):
            info["CFBundleIconFile"] = "AppIcon.icns"
        else:
            print("bw2.app: white.exe has no usable icon; continuing without an app icon", flush=True)
        (contents / "Info.plist").write_bytes(plistlib.dumps(info))
        r = subprocess.run([XATTR, "-dr", "com.apple.quarantine", str(part)], capture_output=True, text=True)
        if r.returncode != 0:
            raise PackageError(f"xattr -dr com.apple.quarantine {part} failed (exit {r.returncode}): "
                               f"{r.stderr.strip()}")
        sign_app(part)
        verify_app(part)
        return install_app(part, dest_dir)
    finally:
        _remove(staging_dir)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.app",
                                 description="Package an engine with gamefiles/game and out/d3dx9_25.dll.")
    ap.add_argument("--engine", required=True, type=Path)
    ap.add_argument("--ffmpeg", required=True, type=Path, help="the LGPL FFmpeg prefix (bw2.ffmpeg)")
    ap.add_argument("--out", required=True, type=Path, help="the folder that gets the app")
    ap.add_argument("--app-version")
    ap.add_argument("--builder-version")
    a = ap.parse_args(argv)
    try:
        v = version()
        app = package_app(engine=a.engine, d3dx_dll=REPO / "out" / DLL, ffmpeg_prefix=a.ffmpeg,
                          game=GAMEFILES / "game", dest_dir=a.out, app_version=a.app_version or v,
                          builder_version=a.builder_version or v)
    except (PackageError, gametree.ManifestMismatch, OSError) as e:
        print(f"bw2.app: {e}", file=sys.stderr)
        return 1
    print(app)
    return 0


if __name__ == "__main__":
    sys.exit(main())
