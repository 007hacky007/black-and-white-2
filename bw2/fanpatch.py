"""The unofficial patch 1.42: an Inno Setup installer holding only white.exe."""
import os
from pathlib import Path

from .inno import read_fan_exe

FAN_PATCH_URL = ("https://www.bwgame.net/downloads/"
                 "black-white-2-unofficial-patch-v1-42.1421/download")


def extract_fan_exe(installer: Path, out_dir: Path) -> Path:
    exe = read_fan_exe(Path(installer))     # checks the installer and the exe by SHA-256
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / "white.exe"
    part = out_dir / "white.exe.part"
    try:
        with open(part, "wb") as f:
            f.write(exe)
            f.flush()
            os.fsync(f.fileno())
        part.replace(target)
    finally:
        part.unlink(missing_ok=True)
    return target
