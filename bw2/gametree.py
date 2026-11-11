"""Build the game data tree: 1.0 install + official 1.1 + 1.2 + fan patch 1.42 white.exe.

The tree is built in `<out>.part` and renamed to `<out>` only after it matches the
committed manifest, so an interrupted build is never mistaken for a finished one.

    python -m bw2.gametree [--write-manifest]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from .diskspace import ensure_free
from .fanpatch import extract_fan_exe
from .install import read_manifest, write_manifest
from .patches import patch_inputs
from .paths import GAMEFILES, REPO
from .releases import sha256_file
from .rtpatch import apply_rtp

MANIFEST = REPO / "bw2" / "manifests" / "game-usa-1.2-fan1.42.tsv"

# Group -> destination, from the <TARGETDIR>\... strings in each setup's data1.hdr.
GROUPS = {
    "patch-1.1": [("Credits_English", "Data"), ("Text_English", "Data/Text"),
                  ("Ape_Texture_Fix", "Data/CTR/bape"), ("Readme_English", "")],
    "patch-1.2": [("Credits_English", "Data"), ("Text_English", "Data/Text"),
                  ("Ape_Texture_Fix", "Data/Art/Textures"), ("Readme_English", ""),
                  ("ScriptDLL", "Plugins"), ("Scripts", "Scripts/BW2"),
                  ("ATI_Fix", "Data/Effects"), ("GraphicConfigUpdate", "Data")],
}
# Files the patches add rather than replace (phase 0, official-patches.md).
NEW_FILES = {"t_ape_evil_scalebias.dds", "t_ape_good_scalebias.dds", "battery.dds",
             "readme_v1_1.txt", "readme_v1_2.txt"}


class ManifestMismatch(Exception):
    pass


class UnexpectedNewFile(Exception):
    pass


def _ci_child(parent: Path, name: str) -> Path:
    """parent/name, reusing an existing entry whose name differs only in case."""
    if not parent.is_dir():
        return parent / name
    names = os.listdir(parent)          # the on-disk spelling, also on case-insensitive volumes
    if name in names:
        return parent / name
    low = name.lower()
    return next((parent / n for n in sorted(names) if n.lower() == low), parent / name)


def _ci_dir(root: Path, rel: str) -> Path:
    cur = root
    for part in [p for p in rel.split("/") if p]:
        cur = _ci_child(cur, part)
        cur.mkdir(exist_ok=True)
    return cur


def copy_groups(groups_dir: Path, table, tree: Path) -> tuple[list[str], set[str]]:
    """Copy each group's files into its destination; returns (copied paths, names that were new)."""
    copied, new = [], set()
    for group, dest in table:
        src = Path(groups_dir) / group
        if not src.is_dir():
            raise FileNotFoundError(f"patch group {group} missing in {groups_dir}")
        target_dir = _ci_dir(Path(tree), dest)
        for f in sorted(p for p in src.iterdir() if p.is_file()):
            target = _ci_child(target_dir, f.name)
            if not target.exists():
                new.add(f.name.lower())
            shutil.copyfile(f, target)
            copied.append(target.relative_to(tree).as_posix())
    return copied, new


def _tree_bytes(root: Path) -> int:
    return sum(p.stat().st_size for p in Path(root).rglob("*") if p.is_file())


def _clonefile(src: Path, dst: Path) -> bool:
    """APFS copy-on-write clone of a directory tree; False if the volume cannot."""
    r = subprocess.run(["/bin/cp", "-cR", str(src), str(dst)], capture_output=True)
    if r.returncode != 0:
        shutil.rmtree(dst, ignore_errors=True)
        return False
    return True


def clone_tree(src: Path, dst: Path) -> None:
    if _clonefile(src, dst):
        return
    ensure_free(dst.parent, _tree_bytes(src))
    shutil.copytree(src, dst)


def verify_manifest(root: Path, tsv: Path) -> None:
    want = read_manifest(tsv)
    have = {p.relative_to(root).as_posix(): p for p in Path(root).rglob("*") if p.is_file()}
    problems = [f"missing {r}" for r in sorted(set(want) - set(have))]
    problems += [f"unexpected {r}" for r in sorted(set(have) - set(want))]
    for rel in sorted(set(want) & set(have)):
        size, sha = want[rel]
        if have[rel].stat().st_size != size or sha256_file(have[rel]) != sha:
            problems.append(f"changed {rel}")
    if problems:
        raise ManifestMismatch(f"{len(problems)} difference(s) from {tsv.name}: " + "; ".join(problems[:10]))


def build_game_tree(install: Path, patch11: Path, patch12: Path, fan_installer: Path,
                    out: Path = GAMEFILES / "game", work: Path = GAMEFILES / "patches",
                    write: bool = False, manifest: Path = MANIFEST) -> Path:
    out, work = Path(out), Path(work)
    part = out.with_name(out.name + ".part")
    shutil.rmtree(part, ignore_errors=True)
    clone_tree(Path(install), part)
    for kind, exe in (("patch-1.1", patch11), ("patch-1.2", patch12)):
        tag = kind.split("-")[1].replace(".", "_")
        pi = patch_inputs(exe, kind, work / f"inputs-{tag}")
        r = apply_rtp(pi.dll, pi.rtp, part, log=work / f"rtp-{tag}.log")
        print(f"[game] {kind} RTPatch: {r.modified} modified, {r.added} added, {r.deleted} deleted "
              f"in {r.seconds} s", flush=True)
        copied, new = copy_groups(pi.groups, GROUPS[kind], part)
        unexpected = new - NEW_FILES
        if unexpected:
            raise UnexpectedNewFile(f"{kind} groups add files phase 0 did not expect: {sorted(unexpected)}")
        print(f"[game] {kind} groups: {len(copied)} files, new: {sorted(new)}", flush=True)
    fan = extract_fan_exe(fan_installer, work / "fan-1.42")
    shutil.copyfile(fan, _ci_child(part, "white.exe"))
    if write:
        n = write_manifest(part, manifest)
        print(f"[game] wrote {manifest.name}: {n} files", flush=True)
    else:
        verify_manifest(part, manifest)
    shutil.rmtree(out, ignore_errors=True)
    part.rename(out)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.gametree")
    ap.add_argument("--write-manifest", action="store_true")
    a = ap.parse_args(argv)
    p = GAMEFILES / "patches"
    out = build_game_tree(GAMEFILES / "install", p / "BW2Patch1_1.exe", p / "BW2Patch1_2.exe",
                          p / "B&W2 Fan Patch v1.42 Installer.exe", write=a.write_manifest)
    print(f"[game] {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
