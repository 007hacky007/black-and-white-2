"""Official patches 1.1 and 1.2: InstallShield 11 setups holding a cabinet set."""
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import tools
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
    r = subprocess.run([str(tools.unshield()), "-d", str(disk), "x", "-d", str(groups),
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


SUPPORT_GROUP = "_Support_Language_Independent_OS_Independent_Files"
RTP_NAMES = {"patch-1.1": "USA_V1_0_TO_V1_1_INSTALL.RTP", "patch-1.2": "ALL_V1_1_TO_V1_2_INSTALL.RTP"}


@dataclass
class PatchInputs:
    dll: Path
    rtp: Path
    groups: Path


def _inputs_ok(groups: Path, kind: str) -> PatchInputs | None:
    sup = groups / SUPPORT_GROUP
    dll, rtp = sup / "patchw32.dll", sup / RTP_NAMES[kind]
    if dll.is_file() and rtp.is_file() and identify(dll, kind="patchw32") \
            and identify(rtp, kind="rtp-" + kind.split("-")[1]):
        return PatchInputs(dll, rtp, groups)
    return None


def patch_inputs(patch_exe: Path, kind: str, out: Path) -> PatchInputs:
    """patchw32.dll, the USA/ALL RTPatch diff and the file groups of one official patch."""
    if kind not in RTP_NAMES:
        raise ValueError(f"unknown patch kind {kind}")
    out = Path(out)
    groups, marker = out / "groups", out / ".complete"
    # Reuse only an extraction that finished: unshield writes in place, and a
    # Ctrl-C can leave the support group whole and another group truncated.
    found = _inputs_ok(groups, kind) if marker.is_file() and groups.is_dir() else None
    if found:
        return found
    groups = extract_patch_groups(patch_exe, kind, out)    # clears `out`, marker included
    found = _inputs_ok(groups, kind)
    if not found:
        raise FingerprintMismatch(f"{Path(patch_exe).name}: patchw32.dll or {RTP_NAMES[kind]} "
                                  f"missing or not the known file")
    marker.write_text(kind + "\n")
    return found
