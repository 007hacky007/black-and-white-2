"""The build from discs and patches in one command (spec 8; plan 13 Task 6). The Builder app runs this.

    python -m bw2.builder_cli setup --venv DIR
    python -m bw2.builder_cli download --kind patch-1.1|patch-1.2|fanpatch-installer --dest DIR
    python -m bw2.builder_cli build --input PATH ... --builder-version B [--app-version V]
    python -m bw2.builder_cli clean

Progress goes to stdout as `BW2-EVENT <json>` lines, one object each: {"step": id, "title": t} when a step
starts, {"progress": f} (0 to 1 within the step), {"done": path} at the end, {"error": message} on a failure
(then exit 1). Every other line is plain log text.

`build` checks every input before it writes anything (`classify`): a missing, duplicate, damaged or unknown
input is named with the others in one error. Then the free space, then the steps of STEPS in order. Each step
first removes the .part outputs an interrupted run may have left, so a half-written file is never trusted.
Every subprocess runs with PATH=/usr/bin:/bin:/usr/sbin:/sbin.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import traceback
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from . import disc, ffmpeg, install, tools
from .diskspace import NotEnoughSpace, ensure_free
from .download import PATCH_SOURCES, FingerprintMismatch, fetch, fetch_pcgw
from .fanpatch import FAN_PATCH_URL
from .paths import REPO
from .releases import Release, identify, load_releases, sha256_file
from .workspace import SOURCES

# The build's packages (requirements-builder.txt). The Builder runs `setup` with its bare private Python, before
# they are installed, so only `build` needs them; without them it stops with an error naming the package.
PACKAGES = ("capstone", "pefile", "unicorn")
try:
    from . import app, gametree, pipeline
    from .d3dx import d3dx_dll
    MISSING_PACKAGE = None
except ModuleNotFoundError as e:
    if e.name not in PACKAGES:
        raise
    MISSING_PACKAGE = e.name

ROOT = REPO
FIXED_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
EVENT = "BW2-EVENT "
ZIP_TABLE = REPO / "bw2" / "manifests" / "disc-zips.tsv"
REQUIREMENTS = REPO / "requirements-builder.txt"

STEPS = [("tools", "Building the tools"), ("discs", "Reading the discs"), ("install", "Unpacking the game"),
         ("patches", "Applying patches 1.1 and 1.2"), ("lift", "Translating the game's code"),
         ("compile", "Compiling"), ("package", "Making the app")]
# Each step's share of the bar, from the wall times of the end-to-end build (plan 13 Task 6 Step 6, 2026-11-14:
# 32.6, 12.6, 17.9, 160.1, 125.3, 185.5 and 21.8 s, 555.8 s in all, on a busy Mac), rounded to add up to 100.
WEIGHTS = {"tools": 6, "discs": 2, "install": 3, "patches": 29, "lift": 23, "compile": 33, "package": 4}
# The peak extra bytes a build from the four zips needs in the workspace: the largest fall of the volume's free
# space in Task 6's end-to-end build (10,604,712 KB, other processes included) plus 20%, rounded up. Not the
# workspace's `du` peak (16,345,136 KB): du counts the APFS clones (install to game, game to app) in full, and the
# workspace is always on the APFS home volume. Each step's own ensure_free still fails loudly by name.
REQUIRED_BYTES = 13_100_000_000

PATCH_KINDS = ("patch-1.1", "patch-1.2", "fanpatch-installer")
WHAT = {"patch-1.1": "patch 1.1", "patch-1.2": "patch 1.2", "fanpatch-installer": "fan patch 1.42"}
DOWNLOAD_KINDS = PATCH_KINDS
# Input file types; anything else in a folder is not looked at.
EXTENSIONS = (".zip", ".cue", ".bin", ".iso", ".exe")

# The messages the Builder shows; its Swift side copies this table (Task 7's test_steps_agree compares them).
MESSAGES = {
    "missing": "missing: {what}",
    "duplicate": "duplicate: {what}",
    "unknown": "not a known file: {name}",
    "cue_bin": "{name}: the cue names {bin}, which is not there",
    "not_found": "not found: {name}",
    "space": "not enough free space on {volume}: the build needs {need} bytes and {free} are free",
}


class InputError(Exception):
    def __init__(self, problems: list[str]):
        super().__init__("; ".join(problems))
        self.problems = problems


class LinkError(Exception):
    pass


class CleanError(Exception):
    pass


@dataclass(frozen=True)
class Inputs:
    discs: dict[int, Path]
    patch11: Path
    patch12: Path
    fan: Path


def emit(**fields) -> None:
    print(EVENT + json.dumps(fields), flush=True)


# ---- the inputs --------------------------------------------------------------------------------------------------

def load_zip_table(tsv: Path = ZIP_TABLE) -> dict[int, tuple[str, int, int]]:
    """disc number -> (bin name, bin size, CRC-32) as a zip's central directory gives them."""
    out = {}
    for line in Path(tsv).read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            n, name, size, crc = line.split("\t")
            out[int(n)] = (name, int(size), int(crc, 16))
    return out


def _disc_number(rel: Release | None) -> int | None:
    m = re.search(r"\(Disc (\d+)\)$", rel.name) if rel and rel.kind == "disc" else None
    return int(m.group(1)) if m else None


def _zip_disc(z: Path, table: dict[int, tuple[str, int, int]]) -> int | None:
    """The disc a zip holds, from its central directory alone: one .bin and one .cue, the bin known by name,
    size and CRC-32. The bin's SHA-256 is checked after unpacking, in the discs step."""
    try:
        with zipfile.ZipFile(z) as zf:
            members = [i for i in zf.infolist() if not i.is_dir()]
    except (zipfile.BadZipFile, OSError, EOFError):
        return None
    bins = [i for i in members if i.filename.lower().endswith(".bin")]
    cues = [i for i in members if i.filename.lower().endswith(".cue")]
    if len(bins) != 1 or len(cues) != 1:
        return None
    b = bins[0]
    return next((n for n, (name, size, crc) in table.items()
                 if (Path(b.filename).name, b.file_size, b.CRC) == (name, size, crc)), None)


def _expand(paths: list[Path], problems: list[str]) -> list[Path]:
    """Files given, a folder standing for the files directly in it; each real file once."""
    files, seen = [], set()
    for p in map(Path, paths):
        if p.is_dir():
            found = sorted(c for c in p.iterdir()
                           if c.is_file() and not c.name.startswith(".") and c.suffix.lower() in EXTENSIONS)
        elif p.is_file():
            found = [p]
        else:
            problems.append(MESSAGES["not_found"].format(name=str(p)))
            continue
        for f in found:
            key = f.resolve()
            if key not in seen:
                seen.add(key)
                files.append(key)
    return files


def classify(paths: list[Path], releases: list[Release] | None = None,
             zip_table: dict[int, tuple[str, int, int]] | None = None) -> Inputs:
    """What each input is, before anything is written. Raises InputError naming every problem at once."""
    rels = load_releases() if releases is None else releases
    table = load_zip_table() if zip_table is None else zip_table
    disc_rels = [r for r in rels if r.kind == "disc"]
    patch_rels = [r for r in rels if r.kind in PATCH_KINDS]
    problems: list[str] = []
    files = _expand(paths, problems)
    file_problems: list[str] = []
    discs: dict[int, list[Path]] = {}
    patches: dict[str, list[Path]] = {}
    in_cue = set()
    for f in files:                       # a cue's bin is part of that disc, not a disc of its own
        if f.suffix.lower() == ".cue":
            try:
                b = disc._parse_cue(f)
            except (disc.DiscFormatError, ValueError, OSError):
                continue
            in_cue.add(b.resolve() if b.exists() else b)
    for f in files:
        ext = f.suffix.lower()
        n = None
        if ext == ".zip":
            n = _zip_disc(f, table)
        elif ext == ".cue":
            try:
                b = disc._parse_cue(f)
            except (disc.DiscFormatError, ValueError, OSError):
                b = None
            if b is not None and not b.exists():
                file_problems.append(MESSAGES["cue_bin"].format(name=f.name, bin=b.name))
                continue
            n = _disc_number(identify(b, releases=disc_rels)) if b is not None else None
        elif ext == ".bin" and f in in_cue:
            continue
        elif ext in (".bin", ".iso"):
            n = _disc_number(identify(f, releases=disc_rels))
        else:
            rel = identify(f, releases=patch_rels)
            if rel is not None:
                patches.setdefault(rel.kind, []).append(f)
                continue
        if n is None:
            file_problems.append(MESSAGES["unknown"].format(name=f.name))
        else:
            discs.setdefault(n, []).append(f)
    for n in range(1, 5):
        what = f"disc {n}"
        if n not in discs:
            problems.append(MESSAGES["missing"].format(what=what))
        elif len(discs[n]) > 1:
            problems.append(MESSAGES["duplicate"].format(what=what))
    for kind in PATCH_KINDS:
        if kind not in patches:
            problems.append(MESSAGES["missing"].format(what=WHAT[kind]))
        elif len(patches[kind]) > 1:
            problems.append(MESSAGES["duplicate"].format(what=WHAT[kind]))
    problems += file_problems
    if problems:
        raise InputError(problems)
    return Inputs(discs={n: ps[0] for n, ps in sorted(discs.items())}, patch11=patches["patch-1.1"][0],
                  patch12=patches["patch-1.2"][0], fan=patches["fanpatch-installer"][0])


def patch_names() -> dict[str, str]:
    """kind -> the file name the pipeline reads it by under gamefiles/patches (the release's name)."""
    return {r.kind: r.name for r in load_releases() if r.kind in PATCH_KINDS}


def _link(src: Path, target: Path) -> None:
    src = Path(src).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        target.unlink()                   # a stale link from an earlier build
    elif target.exists():
        if target.is_file() and (os.path.samefile(target, src) or (
                target.stat().st_size == src.stat().st_size and
                sha256_file(target) == sha256_file(src))):
            return                        # the same file, or the same bytes: nothing to link
        raise LinkError(f"{target} is in the way of the link to {src}; run clean first")
    os.symlink(src, target)


def link_inputs(inputs: Inputs, gamefiles: Path, patch_names: dict[str, str] | None = None) -> None:
    """Absolute symlinks into gamefiles (never hard links: the user's images may sit on another drive, and a copy
    would cost gigabytes). A cue and its bin go side by side into discs/<cue stem>/ under their own names, since
    disc._parse_cue finds the bin beside the cue; a lone bin or ISO into discs/<stem>/; each patch into
    patches/<release name>. Zips are left to disc.identify_discs, which unpacks them."""
    gamefiles = Path(gamefiles)
    names = patch_names if patch_names is not None else globals()["patch_names"]()
    for n, p in sorted(inputs.discs.items()):
        ext = p.suffix.lower()
        if ext == ".zip":
            continue
        folder = gamefiles / "discs" / p.stem
        if ext == ".cue":
            b = disc._parse_cue(p)
            _link(p, folder / p.name)
            _link(b, folder / b.name)
        else:
            _link(p, folder / p.name)
    for kind, src in (("patch-1.1", inputs.patch11), ("patch-1.2", inputs.patch12),
                      ("fanpatch-installer", inputs.fan)):
        _link(src, gamefiles / "patches" / names[kind])


def _disc_paths(inputs: Inputs, gamefiles: Path) -> list[Path]:
    """The paths identify_discs reads: each zip as given, each linked image by its link."""
    out = []
    for n, p in sorted(inputs.discs.items()):
        out.append(p if p.suffix.lower() == ".zip" else gamefiles / "discs" / p.stem / p.name)
    return out


# ---- the space check ---------------------------------------------------------------------------------------------

def volume_of(path: Path) -> str:
    p = Path(path).absolute()
    while not p.exists():
        p = p.parent
    p = p.resolve()
    while not os.path.ismount(p) and p != p.parent:
        p = p.parent
    return str(p)


def required_bytes() -> int:
    """REQUIRED_BYTES, or BW2_REQUIRED_BYTES when set: a test switch for the end-to-end builds on a Mac whose free
    space is short of the measured need (the Builder never sets it)."""
    v = os.environ.get("BW2_REQUIRED_BYTES")
    if v is None:
        return REQUIRED_BYTES
    if not v.isdigit():
        raise ValueError(f"BW2_REQUIRED_BYTES must be a number of bytes, not {v!r}")
    return int(v)


def check_space(root: Path, need: int | None = None) -> None:
    need = required_bytes() if need is None else need
    try:
        ensure_free(root, need)
    except NotEnoughSpace:
        p = Path(root)
        while not p.exists():
            p = p.parent
        free = shutil.disk_usage(p).free
        raise NotEnoughSpace(MESSAGES["space"].format(volume=volume_of(root), need=need, free=free)) from None


# ---- the steps ---------------------------------------------------------------------------------------------------

@dataclass
class Context:
    root: Path
    inputs: Inputs
    app_version: str
    builder_version: str
    discs: dict[int, Path] = field(default_factory=dict)
    app: Path | None = None

    @property
    def gamefiles(self) -> Path:
        return self.root / "gamefiles"

    @property
    def out(self) -> Path:
        return self.root / "out"

    @property
    def ffmpeg_prefix(self) -> Path:
        return self.out / "tools" / ffmpeg.LGPL_PREFIX.name


def _remove(p: Path) -> None:
    if p.is_dir() and not p.is_symlink():
        shutil.rmtree(p)
    elif os.path.lexists(p):
        p.unlink()


def _clear(*paths: Path) -> None:
    for p in paths:
        _remove(p)


def _step_tools(ctx: Context) -> None:
    tools_dir = ctx.out / "tools"
    pre = ctx.ffmpeg_prefix
    _clear(tools_dir / "unshield.part", pre.with_name(pre.name + ".part"), pre.with_name(pre.name + ".src.part"))
    tools.unshield(tools_dir)
    emit(progress=0.2)
    ffmpeg.build_ffmpeg(pre, tools_dir)


def _step_discs(ctx: Context) -> None:
    root = ctx.gamefiles / "discs"
    keep = {p.stem for p in ctx.inputs.discs.values()}
    if root.is_dir():                     # an earlier build's images would be found again by d3dx's scan
        for child in sorted(root.iterdir()):
            if child.name not in keep:
                _remove(child)
            elif child.is_dir() and not child.is_symlink():
                _clear(*(p for p in child.iterdir() if p.name.endswith(".part")))
    link_inputs(ctx.inputs, ctx.gamefiles)
    emit(progress=0.1)
    found = disc.identify_discs(_disc_paths(ctx.inputs, ctx.gamefiles))
    if sorted(found) != [1, 2, 3, 4]:
        raise disc.DiscSetError(f"the discs identify as {sorted(found)}, not 1 to 4")
    ctx.discs = found


def _step_install(ctx: Context) -> None:
    _clear(ctx.gamefiles / "install.part")
    install.extract_install(ctx.discs, ctx.gamefiles / "install")


def _check_white_exe(p: Path) -> None:
    if not identify(p, kind="white-exe"):
        raise FingerprintMismatch(f"{p} is not the fan patch's white.exe 1.42")


def _step_patches(ctx: Context) -> None:
    g = ctx.gamefiles
    _clear(g / "game.part", g / "exe" / "white.exe.part")
    names = patch_names()
    pdir = g / "patches"
    game = gametree.build_game_tree(g / "install", pdir / names["patch-1.1"], pdir / names["patch-1.2"],
                                    pdir / names["fanpatch-installer"], out=g / "game", work=pdir)
    emit(progress=0.9)
    exe_dir = g / "exe"
    exe_dir.mkdir(parents=True, exist_ok=True)
    part = exe_dir / "white.exe.part"
    shutil.copyfile(gametree._ci_child(Path(game), "white.exe"), part)
    _check_white_exe(part)
    part.replace(exe_dir / "white.exe")
    d3dx_dll(g)


def _step_lift(ctx: Context) -> None:
    pipeline.run_pipeline(gen=ctx.root / "gen", out=ctx.out / "bw2", lift=True, do_build=False)


def _step_compile(ctx: Context) -> None:
    _clear(ctx.out / "bw2.part")
    os.environ["BW2_FFMPEG_PREFIX"] = str(ctx.ffmpeg_prefix)
    pipeline.run_pipeline(gen=ctx.root / "gen", out=ctx.out / "bw2", lift=False, do_build=True)


def _step_package(ctx: Context) -> None:
    _clear(ctx.out / (app.APP_NAME + ".part"))
    ctx.app = app.package_app(engine=ctx.out / "bw2", d3dx_dll=ctx.out / app.DLL, ffmpeg_prefix=ctx.ffmpeg_prefix,
                              game=ctx.gamefiles / "game", dest_dir=ctx.out, app_version=ctx.app_version,
                              builder_version=ctx.builder_version)


STEP_FUNCS = {"tools": _step_tools, "discs": _step_discs, "install": _step_install, "patches": _step_patches,
              "lift": _step_lift, "compile": _step_compile, "package": _step_package}


# ---- the commands ------------------------------------------------------------------------------------------------

def _fail(message: str) -> int:
    emit(error=message)
    return 1


def cmd_build(a) -> int:
    if MISSING_PACKAGE:
        return _fail(f"the build's package {MISSING_PACKAGE} is not installed: run setup first")
    try:
        inputs = classify(a.input)
    except InputError as e:
        return _fail(str(e))
    try:
        app_version = a.app_version or app.version()
        if (ROOT / SOURCES).is_file():
            # A cancelled or failed build's files are not reused: removing them first means they never count
            # against the space check, which then measures what REQUIRED_BYTES was measured for (Builder row 4).
            print("[builder] removing an earlier build's files (gamefiles, gen, out but out/tools)", flush=True)
            clean(ROOT)
        check_space(ROOT)
    except (app.PackageError, NotEnoughSpace, CleanError, OSError, ValueError) as e:
        return _fail(str(e))
    os.environ["BW2_FFMPEG_PREFIX"] = str(ffmpeg.LGPL_PREFIX)
    ctx = Context(root=ROOT, inputs=inputs, app_version=app_version, builder_version=a.builder_version)
    t_all = time.monotonic()
    for sid, title in STEPS:
        emit(step=sid, title=title)
        emit(progress=0.0)
        t0 = time.monotonic()
        try:
            STEP_FUNCS[sid](ctx)
        except Exception as e:
            traceback.print_exc(file=sys.stderr)
            sys.stderr.flush()
            return _fail(f"{title}: {e}")
        print(f"[builder] {sid}: {time.monotonic() - t0:.1f} s", flush=True)
        emit(progress=1.0)
    print(f"[builder] all steps: {time.monotonic() - t_all:.1f} s", flush=True)
    emit(done=str(ctx.app))
    return 0


def cmd_setup(a) -> int:
    venv = Path(a.venv).absolute()
    steps = [[sys.executable, "-m", "venv", str(venv)],
             [str(venv / "bin" / "python"), "-m", "pip", "install", "--require-hashes", "--only-binary=:all:",
              "--no-deps", "-r", str(REQUIREMENTS)]]
    for i, argv in enumerate(steps):
        print("[setup] " + " ".join(argv), flush=True)
        r = subprocess.run(argv)
        if r.returncode != 0:
            return _fail(f"{' '.join(argv[:4])} failed (exit {r.returncode})")
        emit(progress=(i + 1) / (len(steps) + 1))
    digest = hashlib.sha256(REQUIREMENTS.read_bytes()).hexdigest()
    part = venv / ".build-requirements-sha256.part"
    part.write_text(digest + "\n")
    part.replace(venv / ".build-requirements-sha256")
    emit(done=str(venv))
    return 0


def cmd_download(a) -> int:
    rows = [r for r in load_releases() if r.kind == a.kind]
    if len(rows) != 1:
        return _fail(f"{a.kind}: {len(rows)} known releases, expected 1")
    expected = rows[0]
    dest = Path(a.dest) / expected.name
    last = [-1.0]

    def progress(f: float) -> None:
        if f >= 1.0 or f - last[0] >= 0.01:
            last[0] = f
            emit(progress=round(f, 4))

    try:
        if a.kind in PATCH_SOURCES:
            page, r = PATCH_SOURCES[a.kind]
            got = fetch_pcgw(page, r, dest, expected, progress=progress)
        else:
            got = fetch(FAN_PATCH_URL, dest, expected, progress=progress)
    except Exception as e:
        traceback.print_exc(file=sys.stderr)
        return _fail(str(e))
    emit(done=str(got))
    return 0


def clean(root: Path) -> None:
    """Removes gamefiles/, gen/ and everything in out/ but out/tools/ of a Builder workspace. A link is removed,
    never followed. Only a workspace (a copy with SOURCES.txt) is cleaned, never a development tree."""
    root = Path(root)
    if not (root / SOURCES).is_file():
        raise CleanError(f"{root} has no {SOURCES}: only a Builder workspace is cleaned")
    _clear(root / "gamefiles", root / "gen")
    out = root / "out"
    if out.is_dir() and not out.is_symlink():
        for child in sorted(out.iterdir()):
            if child.name != "tools":
                _remove(child)
    elif os.path.lexists(out):
        out.unlink()


def cmd_clean(a) -> int:
    try:
        clean(ROOT)
    except (CleanError, OSError) as e:
        return _fail(str(e))
    emit(done=str(ROOT))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.builder_cli")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--venv", required=True, type=Path)
    d = sub.add_parser("download")
    d.add_argument("--kind", required=True, choices=DOWNLOAD_KINDS)
    d.add_argument("--dest", required=True, type=Path)
    b = sub.add_parser("build")
    b.add_argument("--input", required=True, nargs="+", type=Path)
    b.add_argument("--builder-version", required=True)
    b.add_argument("--app-version")
    sub.add_parser("clean")
    a = ap.parse_args(argv)
    os.environ["PATH"] = FIXED_PATH          # every subprocess of every step inherits it
    return {"setup": cmd_setup, "download": cmd_download, "build": cmd_build, "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
