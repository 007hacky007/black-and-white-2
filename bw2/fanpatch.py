"""The unofficial patch 1.42: an Inno Setup installer holding only white.exe."""
import shutil
import subprocess
import tempfile
from pathlib import Path

from .download import FingerprintMismatch
from .releases import identify, load_releases

FAN_PATCH_URL = ("https://www.bwgame.net/downloads/"
                 "black-white-2-unofficial-patch-v1-42.1421/download")


def _check(path: Path, kind: str) -> None:
    rels = [r for r in load_releases() if r.kind == kind]
    if not identify(path, releases=rels):
        raise FingerprintMismatch(f"{path.name} is not a known {kind}")


def extract_fan_exe(installer: Path, out_dir: Path) -> Path:
    _check(installer, "fanpatch-installer")
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out_dir) as tmp:
        subprocess.run(["innoextract", "-s", "-d", tmp, str(installer)], check=True,
                       capture_output=True)
        exe = Path(tmp) / "code$GetDestDir" / "white.exe"
        _check(exe, "white-exe")
        target = out_dir / "white.exe"
        shutil.move(str(exe), target)
    return target
