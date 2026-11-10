"""Official patches 1.1 and 1.2: InstallShield 11 setups holding a cabinet set."""
import shutil
import subprocess
from pathlib import Path

from .download import FingerprintMismatch
from .is11 import split_overlay
from .releases import identify, load_releases


def _check(path: Path, kind: str) -> None:
    if not identify(path, releases=[r for r in load_releases() if r.kind == kind]):
        raise FingerprintMismatch(f"{Path(path).name} is not a known {kind}")


def extract_patch_groups(patch_exe: Path, kind: str, out: Path) -> Path:
    if kind not in ("patch-1.1", "patch-1.2"):
        raise ValueError(f"unknown patch kind {kind}")
    _check(patch_exe, kind)
    out = Path(out)
    setup, groups = out / "setup", out / "groups"
    shutil.rmtree(out, ignore_errors=True)
    split_overlay(patch_exe, setup)
    disk = setup / "Disk1"
    r = subprocess.run(["unshield", "-d", str(disk), "x", "-d", str(groups),
                        str(disk / "data1.hdr")], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"unshield failed on {Path(patch_exe).name}: {r.stderr[-1000:]}")
    return groups


def script_dll(patch12_exe: Path, out_dir: Path) -> Path:
    out_dir = Path(out_dir)
    target = out_dir / "ScriptLibraryR.dll"
    if target.exists() and identify(target, kind="script-dll"):
        return target
    groups = extract_patch_groups(patch12_exe, "patch-1.2", out_dir / "patch-1.2")
    src = groups / "ScriptDLL" / "ScriptLibraryR.dll"
    _check(src, "script-dll")
    shutil.copyfile(src, target)
    return target
