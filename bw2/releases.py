"""Known files by size and SHA-256: discs, patches, executables."""
import hashlib
from dataclasses import dataclass
from pathlib import Path

DEFAULT_TSV = Path(__file__).with_name("known_releases.tsv")


@dataclass(frozen=True)
class Release:
    kind: str
    name: str
    size: int
    sha256: str
    note: str


def load_releases(tsv: Path = DEFAULT_TSV) -> list[Release]:
    out = []
    for line in tsv.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        kind, name, size, sha, *note = line.split("\t")
        out.append(Release(kind, name, int(size), sha.lower(), note[0] if note else ""))
    return out


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def identify(path: Path, kind: str | None = None,
             releases: list[Release] | None = None) -> Release | None:
    """The known release this file is, or None. Hashes only on a size match."""
    rels = load_releases() if releases is None else releases
    size = Path(path).stat().st_size
    cands = [r for r in rels if r.size == size and (kind is None or r.kind == kind)]
    if not cands:
        return None
    digest = sha256_file(path)
    return next((r for r in cands if r.sha256 == digest), None)
