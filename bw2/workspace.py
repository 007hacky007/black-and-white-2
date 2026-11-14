"""The Builder's workspace: a copy of the git-tracked sources the build needs, and nothing else.

Every step writes under gamefiles/, gen/ and out/ of the tree it runs from (bw2/paths.py), so a copy of
the sources is a complete build tree. The list comes from `git ls-files` in a work tree, or from the
SOURCES.txt a copy carries, so a copy can copy itself again (BW2 Builder.app holds one).
"""
import os
import shutil
import subprocess
from pathlib import Path, PurePosixPath

from .paths import REPO

SOURCES = "SOURCES.txt"
DIRS = ("bw2", "runtime", "third_party", "tools/unshield")
FILES = ("requirements-builder.txt", "VERSION")
NEVER = ("gamefiles/", "gen/", "out/", "tests/", "docs/", ".superpowers/")


class SourcesError(Exception):
    pass


def _wanted(rel: str) -> bool:
    return rel in FILES or any(rel.startswith(d + "/") for d in DIRS)


def _check(rel: str, where: str) -> str:
    p = PurePosixPath(rel)
    if p.is_absolute() or ".." in p.parts or rel.startswith(NEVER) or not _wanted(rel):
        raise SourcesError(f"{where}: {rel!r} is not a source path")
    return rel


def is_git_top(root: Path) -> bool:
    """True if root is the top of a git work tree."""
    try:
        r = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    except OSError:
        return False
    return r.returncode == 0 and Path(r.stdout.strip()).resolve() == Path(root).resolve()


def source_list(root: Path = REPO) -> list[str]:
    """The relative paths to copy: SOURCES.txt of a copy, else `git ls-files` of a work tree. A copy never runs
    git: on a Mac without the Command Line Tools /usr/bin/git is a shim that pops Apple's installer."""
    root = Path(root)
    manifest = root / SOURCES
    if manifest.is_file():
        rels = [ln for ln in manifest.read_text().splitlines() if ln.strip()]
        return sorted(_check(r, str(manifest)) for r in rels)
    if not is_git_top(root):
        raise SourcesError(f"{root} is neither a git work tree nor a copy with {SOURCES}")
    r = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", *DIRS, *FILES],
                       capture_output=True, check=True)
    rels = [p for p in r.stdout.decode().split("\0") if p]
    return sorted(_check(p, "git ls-files") for p in rels if _wanted(p))


def copy_sources(dest: Path, root: Path = REPO) -> list[str]:
    """Copies the sources of root into dest and writes dest/SOURCES.txt; returns the relative paths.
    Files an earlier copy listed in dest/SOURCES.txt that the new list lacks are removed."""
    root, dest = Path(root), Path(dest)
    rels = source_list(root)
    missing = [r for r in rels if not os.path.lexists(root / r)]
    if missing:
        raise SourcesError(f"{len(missing)} listed source(s) missing under {root}: {', '.join(missing[:5])}")
    old = dest / SOURCES
    stale = []
    if old.is_file():
        stale = sorted(set(_check(r, str(old)) for r in old.read_text().splitlines() if r.strip()) - set(rels))
    dest.mkdir(parents=True, exist_ok=True)
    for rel in stale:
        (dest / rel).unlink(missing_ok=True)
    for rel in rels:
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if os.path.lexists(target):
            target.unlink()
        shutil.copy2(root / rel, target, follow_symlinks=False)
    part = dest / (SOURCES + ".part")
    part.write_text("".join(r + "\n" for r in rels))
    part.replace(dest / SOURCES)
    return rels
