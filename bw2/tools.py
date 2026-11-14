"""Command-line tools the pipeline builds itself, so that nothing comes from Homebrew.

unshield 1.6.2 (MIT) is vendored unchanged under third_party/unshield; our config header and
the CommonCrypto MD5 shim live under tools/unshield. It is compiled and linked in one clang
call into out/tools/unshield, and compiled again only when its sources or its compile
command line change (the stamp out/tools/unshield.sha256).
"""
import hashlib
import os
import subprocess
from pathlib import Path

from . import build
from .paths import REPO

TOOLS_DIR = REPO / "out" / "tools"
UNSHIELD_SRC = REPO / "third_party" / "unshield"
UNSHIELD_OURS = REPO / "tools" / "unshield"


def _unshield_sources() -> list[Path]:
    return sorted(UNSHIELD_SRC.glob("lib/*.c")) + [UNSHIELD_SRC / "src" / "unshield.c"]


def _unshield_inputs() -> list[Path]:
    """Every file whose content changes the binary: the vendored tree and our headers."""
    files = [p for root in (UNSHIELD_SRC, UNSHIELD_OURS) for p in root.rglob("*") if p.is_file()]
    return sorted(files)


def _unshield_argv(clang: Path, sdk: Path, part: Path) -> list[str]:
    return [str(clang), "-O2", "-mcpu=apple-m1", "-mmacosx-version-min=13.0",
            "-isysroot", str(sdk), "-Wno-deprecated-declarations", "-DHAVE_CONFIG_H",
            "-I", str(UNSHIELD_OURS), "-I", str(UNSHIELD_SRC), "-I", str(UNSHIELD_SRC / "lib"),
            *(str(p) for p in _unshield_sources()), "-lz", "-liconv", "-o", str(part)]


def _stamp(argv: list[str]) -> str:
    h = hashlib.sha256()
    for p in _unshield_inputs():
        h.update(p.relative_to(REPO).as_posix().encode() + b"\0")
        h.update(hashlib.sha256(p.read_bytes()).digest())
    h.update(b"\0argv\0")
    for a in argv:
        h.update(a.encode() + b"\0")
    return h.hexdigest()


def unshield(out_dir: Path = TOOLS_DIR) -> Path:
    """out_dir/unshield, compiled from the vendored source when missing or out of date."""
    out_dir = Path(out_dir)
    exe, stamp_file = out_dir / "unshield", out_dir / "unshield.sha256"
    part = out_dir / "unshield.part"
    argv = _unshield_argv(build.check_tools(), build.sdk_path(), part)
    want = _stamp(argv)
    have = stamp_file.read_text().strip() if stamp_file.is_file() else ""
    if exe.is_file() and os.access(exe, os.X_OK) and have == want:
        return exe
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp_file.unlink(missing_ok=True)
    part.unlink(missing_ok=True)
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0 or not part.is_file():
        part.unlink(missing_ok=True)
        raise build.BuildError(f"compiling unshield failed (exit {r.returncode}):\n"
                               + build._tail(r.stdout + r.stderr))
    part.chmod(0o755)
    part.replace(exe)
    stamp_file.write_text(want + "\n")
    return exe
