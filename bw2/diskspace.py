"""Fail before writing when the disk cannot hold what we are about to write."""
import shutil
from pathlib import Path


class NotEnoughSpace(Exception):
    pass


def ensure_free(path: Path, needed: int) -> None:
    p = Path(path)
    while not p.exists():
        p = p.parent
    free = shutil.disk_usage(p).free
    if free < needed:
        raise NotEnoughSpace(f"{path}: need {needed} bytes, only {free} free")
