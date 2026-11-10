"""Lay out the game as the Windows installer would, from the four discs.

The discs carry one InstallShield 9 set split over five cabinets
(data1.hdr + data1..5.cab). unshield (MIT) extracts it with every file's MD5
checked; pcrecomp's isextract.py fails on files that span two cabinets.
"""
import shutil
import subprocess
from pathlib import Path

from . import disc
from .paths import GAMEFILES
from .releases import sha256_file


class UnknownGroup(Exception):
    pass


class ExtractError(Exception):
    pass


# unshield -D 2 group directory -> destination under the install root.
# None: installer engine/support files, not part of the installed game.
# Destinations other than "." are inferred from the paths white.exe opens
# (see docs/research/install-tree.md).
GROUP_DEST: dict[str, str | None] = {
    "MainApp": ".",
    "White": ".",
    "SplashEnglish": "Data/Splash",
    "CreditsEnglish": "Data",
    "TextEnglish": "Data/Text",
    "SpeechEnglish": "Audio/Dialogue",
    "MusicEnglish": "Audio/Music/Cutscene",
    "Infocdat": "Scripts",
    "ReadmeEnglish": ".",
}
_SKIP_PREFIXES = ("_Engine_", "_Support_")

IS_FILES = ("data1.hdr", "data1.cab", "data2.cab", "data3.cab", "data4.cab", "data5.cab",
            "layout.bin", "setup.inx")


def _merge(src: Path, dst: Path) -> None:
    for p in sorted(src.rglob("*")):
        if p.is_file():
            t = dst / p.relative_to(src)
            if t.exists():
                raise ExtractError(f"two groups write {t}")
            t.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(p), t)


def layout_groups(src: Path, out: Path) -> None:
    """Move each unshield group directory under `src` to its destination in `out`."""
    groups = sorted(p for p in src.iterdir() if p.is_dir())
    unknown = [g.name for g in groups
               if g.name not in GROUP_DEST and not g.name.startswith(_SKIP_PREFIXES)]
    if unknown:
        raise UnknownGroup("unknown InstallShield groups: " + ", ".join(unknown))
    out.mkdir(parents=True, exist_ok=True)
    for g in groups:
        dest = GROUP_DEST.get(g.name)
        if dest is not None:
            _merge(g, out / dest)


def gather(discs: dict[int, Path], dest: Path) -> Path:
    """Copy the InstallShield files of all discs into one flat directory."""
    dest.mkdir(parents=True, exist_ok=True)
    for n in sorted(discs):
        with disc.open_disc(discs[n]) as d:
            for e in disc.list_files(d):
                name = e.path.lower()
                if e.is_dir or "/" in name or name not in IS_FILES:
                    continue
                t = dest / name
                if not (t.exists() and t.stat().st_size == e.size):
                    disc.extract(d, e, t)
    missing = [f for f in IS_FILES if not (dest / f).exists()]
    if missing:
        raise ExtractError("installer files missing from the discs: " + ", ".join(missing))
    return dest


def extract_install(discs: dict[int, Path], out: Path = GAMEFILES / "install") -> Path:
    is_dir = gather(discs, GAMEFILES / "is")
    part = out.with_name(out.name + ".part")
    shutil.rmtree(part, ignore_errors=True)
    raw = part / "_groups"
    r = subprocess.run(["unshield", "-d", str(is_dir), "-D", "2", "x", "-d", str(raw),
                        str(is_dir / "data1.hdr")], capture_output=True, text=True)
    bad = [l for l in (r.stdout + r.stderr).splitlines()
           if not l.lstrip().startswith("extracting:")
           and ("fail" in l.lower() or "error" in l.lower())]
    if r.returncode != 0 or bad:
        raise ExtractError("unshield failed:\n" + "\n".join(bad[:20] or [r.stderr[-2000:]]))
    tree = part / "tree"
    layout_groups(raw, tree)
    shutil.rmtree(raw)
    shutil.rmtree(out, ignore_errors=True)
    tree.rename(out)
    shutil.rmtree(part)
    return out


def write_manifest(root: Path, tsv: Path) -> int:
    rows = sorted((p.relative_to(root).as_posix(), p.stat().st_size, sha256_file(p))
                  for p in root.rglob("*") if p.is_file())
    tsv.parent.mkdir(parents=True, exist_ok=True)
    tsv.write_text("# path\tsize\tsha256\n" +
                   "".join(f"{r}\t{s}\t{h}\n" for r, s, h in rows))
    return len(rows)


def read_manifest(tsv: Path) -> dict[str, tuple[int, str]]:
    out = {}
    for line in tsv.read_text().splitlines():
        if line and not line.startswith("#"):
            rel, size, sha = line.split("\t")
            out[rel] = (int(size), sha)
    return out
