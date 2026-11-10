"""Downloads pinned by size and SHA-256. Nothing half-written ever looks valid."""
import urllib.request
from pathlib import Path

from .releases import Release, identify

USER_AGENT = "bw2-builder/0"


class FingerprintMismatch(Exception):
    pass


def fetch(url: str, dest: Path, expected: Release) -> Path:
    dest = Path(dest)
    if dest.exists() and identify(dest, releases=[expected]):
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as f:
            while chunk := r.read(1 << 20):
                f.write(chunk)
        if not identify(part, releases=[expected]):
            raise FingerprintMismatch(f"{url}: download does not match {expected.name} "
                                      f"({expected.size} bytes, sha256 {expected.sha256})")
        part.replace(dest)
        return dest
    finally:
        part.unlink(missing_ok=True)
