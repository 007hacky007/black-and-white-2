"""Builders for synthetic test inputs. None of them touch game files."""
import subprocess
import zipfile
from pathlib import Path

STEM = "Black & White 2 (USA) (Disc 1)"


def make_iso(tmp: Path, files: dict[str, bytes]) -> Path:
    src = tmp / "isosrc"
    for rel, data in files.items():
        p = src / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    iso = tmp / f"{STEM}.iso"
    subprocess.run(["hdiutil", "makehybrid", "-quiet", "-iso", "-o", str(iso), str(src)],
                   check=True)
    return iso


def iso_to_bincue(iso: Path, stem: str = STEM, mode: str = "MODE1/2352") -> Path:
    d = iso.parent / "bincue"
    d.mkdir(exist_ok=True)
    data = iso.read_bytes()
    with open(d / f"{stem}.bin", "wb") as f:
        for i in range(0, len(data), 2048):
            f.write(b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00" + b"\x00\x02\x00\x01")
            f.write(data[i:i + 2048].ljust(2048, b"\x00"))
            f.write(b"\x00" * 288)
    cue = d / f"{stem}.cue"
    cue.write_text(f'FILE "{stem}.bin" BINARY\n  TRACK 01 {mode}\n    INDEX 01 00:00:00\n')
    return cue


def zip_bincue(cue: Path) -> Path:
    z = cue.parent.parent / f"{cue.stem}.zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(cue.with_suffix(".bin"), cue.with_suffix(".bin").name)
        zf.write(cue, cue.name)
    return z
