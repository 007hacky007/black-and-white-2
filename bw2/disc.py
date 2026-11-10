"""Disc images: zip of BIN/CUE, BIN/CUE (MODE1/2352), ISO; and ISO 9660 reading."""
import argparse
import re
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

from .diskspace import ensure_free
from .paths import GAMEFILES
from .releases import Release, identify

RAW, COOKED, RAW_DATA_OFFSET = 2352, 2048, 16


class DiscFormatError(Exception):
    pass


class DiscSetError(Exception):
    pass


class DiscImage:
    def __init__(self, path: Path, sector_size: int, data_offset: int):
        self.path = Path(path)
        self._f = open(self.path, "rb")
        self._size, self._off = sector_size, data_offset
        self.sectors = self.path.stat().st_size // sector_size

    def _short(self, lba: int) -> DiscFormatError:
        return DiscFormatError(f"{self.path.name}: image ends early (short read at LBA {lba})")

    def read_sector(self, lba: int) -> bytes:
        self._f.seek(lba * self._size + self._off)
        data = self._f.read(COOKED)
        if len(data) < COOKED:
            raise self._short(lba)
        return data

    def read(self, lba: int, length: int) -> bytes:
        if self._size == COOKED:
            self._f.seek(lba * COOKED)
            data = self._f.read(length)
            if len(data) < length:
                raise self._short(lba + len(data) // COOKED)
            return data
        out = bytearray()
        while len(out) < length:
            out += self.read_sector(lba)
            lba += 1
        return bytes(out[:length])

    def close(self) -> None:
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _parse_cue(cue: Path) -> Path:
    text = cue.read_text(errors="replace")
    files = re.findall(r'^\s*FILE\s+"([^"]+)"', text, re.M)
    tracks = re.findall(r"^\s*TRACK\s+\d+\s+(\S+)", text, re.M)
    if len(files) != 1 or len(tracks) != 1:
        raise DiscFormatError(f"{cue.name}: expected one file and one track, "
                              f"found {len(files)} files and {len(tracks)} tracks")
    if tracks[0].upper() != "MODE1/2352":
        raise DiscFormatError(f"{cue.name}: track mode {tracks[0]} is not supported")
    return cue.with_name(files[0])


def _zip_plan(zf: zipfile.ZipFile, zpath: Path):
    """(targets, still-to-unpack) for a zipped BIN/CUE; one folder per zip."""
    members = [i for i in zf.infolist() if not i.is_dir()]
    bins = [i for i in members if i.filename.lower().endswith(".bin")]
    cues = [i for i in members if i.filename.lower().endswith(".cue")]
    if len(bins) != 1 or len(cues) != 1:
        raise DiscFormatError(f"{zpath.name}: expected one .bin and one .cue")
    out = GAMEFILES / "discs" / Path(zpath).stem
    targets = [(i, out / Path(i.filename).name) for i in (cues[0], bins[0])]
    todo = [(i, t) for i, t in targets if not (t.exists() and t.stat().st_size == i.file_size)]
    return targets, todo


def pending_unpack_bytes(zpath: Path) -> int:
    with zipfile.ZipFile(zpath) as zf:
        return sum(i.file_size for i, _ in _zip_plan(zf, zpath)[1])


def unpacked_bin(zpath: Path) -> Path:
    """Unpack a zipped BIN/CUE once into GAMEFILES/discs/<zip stem>/ and return the .bin."""
    with zipfile.ZipFile(zpath) as zf:
        targets, todo = _zip_plan(zf, zpath)
        out = targets[1][1].parent
        if todo:
            ensure_free(out, sum(i.file_size for i, _ in todo))
            out.mkdir(parents=True, exist_ok=True)
            for info, target in todo:
                part = target.with_name(target.name + ".part")
                with zf.open(info) as src, open(part, "wb") as dst:
                    while chunk := src.read(1 << 20):
                        dst.write(chunk)
                part.rename(target)
        return targets[1][1]


def open_disc(path: Path) -> DiscImage:
    path = Path(path)
    ext = path.suffix.lower()
    if ext == ".zip":
        return open_disc(unpacked_bin(path))
    if ext == ".iso":
        return DiscImage(path, COOKED, 0)
    if ext == ".cue":
        return DiscImage(_parse_cue(path), RAW, RAW_DATA_OFFSET)
    if ext == ".bin":
        cue = path.with_suffix(".cue")
        if cue.exists():
            _parse_cue(cue)
            return DiscImage(path, RAW, RAW_DATA_OFFSET)
        if path.stat().st_size % RAW == 0:
            return DiscImage(path, RAW, RAW_DATA_OFFSET)
        raise DiscFormatError(f"{path.name}: no .cue and size is not a multiple of {RAW}")
    raise DiscFormatError(f"{path.name}: not a .zip, .cue, .bin or .iso")


@dataclass(frozen=True)
class IsoEntry:
    path: str
    lba: int
    size: int
    is_dir: bool


def _records(data: bytes):
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // COOKED + 1) * COOKED
            continue
        yield data[i:i + n]
        i += n


def list_files(disc: DiscImage) -> list[IsoEntry]:
    pvd = disc.read_sector(16)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise DiscFormatError(f"{disc.path.name}: no ISO 9660 primary volume descriptor")
    root = pvd[156:156 + 34]
    out: list[IsoEntry] = []
    stack = [("", struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])]
    seen = set()
    while stack:
        prefix, lba, size = stack.pop()
        if lba in seen:
            continue
        seen.add(lba)
        for r in _records(disc.read(lba, size)):
            nlen = r[32]
            raw = r[33:33 + nlen]
            if raw in (b"\x00", b"\x01"):
                continue
            name = raw.decode("latin-1").split(";")[0].rstrip(".")
            e_lba, e_size = struct.unpack_from("<I", r, 2)[0], struct.unpack_from("<I", r, 10)[0]
            is_dir = bool(r[25] & 2)
            p = f"{prefix}{name}"
            out.append(IsoEntry(p, e_lba, e_size, is_dir))
            if is_dir:
                stack.append((p + "/", e_lba, e_size))
    return sorted(out, key=lambda e: e.path)


def find(entries: list[IsoEntry], path: str) -> IsoEntry | None:
    want = path.strip("/").lower()
    return next((e for e in entries if e.path.lower() == want), None)


def extract(disc: DiscImage, entry: IsoEntry, dest: Path) -> None:
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    try:
        with open(part, "wb") as f:
            left, lba = entry.size, entry.lba
            while left > 0:
                n = min(left, 512 * COOKED)
                f.write(disc.read(lba, n))
                left -= n
                lba += n // COOKED
        if part.stat().st_size != entry.size:
            raise DiscFormatError(f"{entry.path}: wrote {part.stat().st_size} of {entry.size} bytes")
        part.rename(dest)
    finally:
        part.unlink(missing_ok=True)


def identify_discs(paths: list[Path], releases: list[Release] | None = None) -> dict[int, Path]:
    """Map disc number (1-4) to image. Zips and cues resolve to their .bin."""
    paths = [Path(p) for p in paths]
    zips = [p for p in paths if p.suffix.lower() == ".zip"]
    if zips:
        ensure_free(GAMEFILES / "discs", sum(pending_unpack_bytes(z) for z in zips))
    found: dict[int, list[Path]] = {}
    unknown = []
    for p in paths:
        b = unpacked_bin(p) if p.suffix.lower() == ".zip" else (
            _parse_cue(p) if p.suffix.lower() == ".cue" else p)
        rel = identify(b, kind="disc", releases=releases)
        m = re.search(r"Disc (\d+)\)?$", rel.name) if rel else None
        if not m:
            unknown.append(p.name)
            continue
        found.setdefault(int(m.group(1)), []).append(p)
    missing = [n for n in range(1, 5) if n not in found]
    dupes = [n for n, ps in sorted(found.items()) if len(ps) > 1]
    if missing or dupes or unknown:
        parts = []
        if missing:
            parts.append("missing: " + ", ".join(map(str, missing)))
        if dupes:
            parts.append("duplicate: " + ", ".join(map(str, dupes)))
        if unknown:
            parts.append("not a known disc: " + ", ".join(unknown))
        raise DiscSetError("; ".join(parts))
    return {n: ps[0] for n, ps in sorted(found.items())}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.disc")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list")
    ls.add_argument("image", type=Path)
    ex = sub.add_parser("extract")
    ex.add_argument("image", type=Path)
    ex.add_argument("iso_path")
    ex.add_argument("dest", type=Path)
    a = ap.parse_args(argv)
    with open_disc(a.image) as d:
        entries = list_files(d)
        if a.cmd == "list":
            for e in entries:
                print(f"{'d' if e.is_dir else '-'} {e.size:>12} {e.path}")
            return 0
        e = find(entries, a.iso_path)
        if e is None:
            print(f"not found: {a.iso_path}", file=sys.stderr)
            return 1
        extract(d, e, a.dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
