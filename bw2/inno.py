"""The fan patch 1.42's white.exe read straight from its Inno Setup installer.

Facts (plan 13, "Measured" item 3), for the pinned `fanpatch-installer` release
only (`B&W2 Fan Patch v1.42 Installer.exe`, 20,266,576 bytes), which is checked
by SHA-256 before any offset below is trusted:

- `Inno Setup Setup Data (6.0.0) (u)` sits at file offset 751,376; there is one
  file-data chunk, whose magic `zlb\\x1a` is at FAN_CHUNK (876,032).
- After the magic: one LZMA2 dictionary-size byte, then a raw LZMA2 stream that
  decodes to 38,054,164 bytes (1,109,041 bytes of the file follow its end).
- The only `MZ` header in the decoded data is at FAN_EXE_OFFSET (17,226,004);
  the exe runs to the end (FAN_EXE_SIZE, 20,828,160 bytes).
- As stored, the exe has Inno Setup 5.2+'s call-instruction transform applied
  (SHA-256 c73141fd...3144); call_decode undoes it, giving the `white-exe`
  release. The transform works per 64 KiB block of the file's data: an E8/E9
  byte followed by a 4-byte operand whose high byte is 00 or FF has the low
  24 bits made relative to (offset + 5) & 0xFFFFFF, and the high byte flipped
  when the decoded value's bit 23 is set; an operand that would cross the
  block's end is left alone, and the 4 bytes are skipped either way.

No innoextract: Python's lzma and this file do it all.
"""
import hashlib
import lzma
import re
from pathlib import Path

from .download import FingerprintMismatch
from .releases import identify, load_releases

FAN_CHUNK = 876032
FAN_EXE_OFFSET = 17226004
FAN_EXE_SIZE = 20828160
CHUNK_MAGIC = b"zlb\x1a"

_CALL = re.compile(b"[\xE8\xE9]")


def call_decode(data: bytes, block: int = 0x10000) -> bytes:
    """Undo Inno Setup 5.2+'s call-instruction transform over a whole file's data."""
    b = bytearray(data)
    n = len(b)
    for base in range(0, n, block):
        end = base + min(block, n - base) - 4    # opcode positions p < end
        p = base
        while True:
            m = _CALL.search(b, p, end) if p < end else None
            if m is None:
                break
            p = m.start() + 1
            if b[p + 3] in (0x00, 0xFF):
                addr = (p + 4) & 0xFFFFFF
                rel = ((b[p] | b[p + 1] << 8 | b[p + 2] << 16) - addr) & 0xFFFFFF
                if rel & 0x800000:
                    b[p + 3] ^= 0xFF
                b[p] = rel & 0xFF
                b[p + 1] = (rel >> 8) & 0xFF
                b[p + 2] = rel >> 16
            p += 4
    return bytes(b)


def _dict_size(d: int) -> int:
    """LZMA2's dictionary-size byte (xz's lzma2 property)."""
    if d > 40:
        raise ValueError(f"LZMA2 dictionary byte {d} is over 40")
    if d == 40:
        return 0xFFFFFFFF
    return (2 | (d & 1)) << (d // 2 + 11)


def _release(kind: str):
    return [r for r in load_releases() if r.kind == kind]


def read_fan_exe(installer: Path) -> bytes:
    """white.exe 1.42's bytes, or FingerprintMismatch naming the file and the stage."""
    installer = Path(installer)
    name = installer.name
    if not identify(installer, releases=_release("fanpatch-installer")):
        raise FingerprintMismatch(f"{name}: installer: not the known fanpatch-installer release")
    with open(installer, "rb") as f:
        f.seek(FAN_CHUNK)
        head = f.read(len(CHUNK_MAGIC) + 1)
        rest = f.read()
    if head[:4] != CHUNK_MAGIC:
        raise FingerprintMismatch(f"{name}: magic: no {CHUNK_MAGIC!r} at offset {FAN_CHUNK}")
    try:
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA2, "dict_size": max(_dict_size(head[4]), 4096)}])
        data = dec.decompress(rest)
    except (ValueError, lzma.LZMAError) as e:
        raise FingerprintMismatch(f"{name}: stream: LZMA2 at offset {FAN_CHUNK + 5}: {e}") from e
    if not dec.eof or len(data) < FAN_EXE_OFFSET + FAN_EXE_SIZE:
        raise FingerprintMismatch(f"{name}: stream: {len(data)} bytes, ended {dec.eof}; "
                                  f"want at least {FAN_EXE_OFFSET + FAN_EXE_SIZE}")
    exe = call_decode(data[FAN_EXE_OFFSET:FAN_EXE_OFFSET + FAN_EXE_SIZE])
    want = _release("white-exe")[0]
    if len(exe) != want.size or hashlib.sha256(exe).hexdigest() != want.sha256:
        raise FingerprintMismatch(f"{name}: exe: the decoded exe is not the known white-exe release")
    return exe
