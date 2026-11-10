"""Classify the bytes of .text that no catalogued function covers."""
import struct
from dataclasses import dataclass
from pathlib import Path

import capstone
import pefile

PAD = {0xCC, 0x90, 0x00}
PROLOGUES = (b"\x55\x8b\xec", b"\x83\xec", b"\x81\xec")


@dataclass(frozen=True)
class Gap:
    lo: int
    hi: int
    kind: str  # "int3" | "align" | "code" | "data" | "unknown"


def _sections(pe):
    base = pe.OPTIONAL_HEADER.ImageBase
    for s in pe.sections:
        yield s.Name.split(b"\0")[0].decode("latin-1"), base + s.VirtualAddress, s.get_data()


def gaps(catalog: Path, exe: Path) -> list[Gap]:
    from .discover import _union, load_functions
    pe = pefile.PE(str(exe), fast_load=True)
    secs = list(_sections(pe))
    tname, tva, tdata = next(s for s in secs if s[0] == ".text")
    tend = tva + len(tdata)
    cov = _union([(max(f["address"], tva), min(f["end"], tend))
                  for f in load_functions(catalog) if f["end"] > f["address"]])
    raw, cur = [], tva
    for a, b in cov:
        if a > cur:
            raw.append((cur, a))
        cur = max(cur, b)
    if cur < tend:
        raw.append((cur, tend))
    covered = lambda v: any(a <= v < b for a, b in cov)
    branch_targets = set()
    for i in range(len(tdata) - 4):
        if tdata[i] in (0xE8, 0xE9) and covered(tva + i):
            branch_targets.add((tva + i + 5 + struct.unpack_from("<i", tdata, i + 1)[0]) & 0xFFFFFFFF)
    data_ptrs = set()
    for name, va, data in secs:
        if name in (".rdata", ".data"):
            for i in range(0, len(data) - 3, 4):
                data_ptrs.add(struct.unpack_from("<I", data, i)[0])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out = []
    for lo, hi in raw:
        b = tdata[lo - tva:hi - tva]
        if all(x == 0xCC for x in b) and len(b) >= 16:
            out.append(Gap(lo, hi, "int3"))
            continue
        if len(b) < 16 and all(x in PAD for x in b):
            out.append(Gap(lo, hi, "align"))
            continue
        lead = 0
        while lead < len(b) and b[lead] in (0xCC, 0x90):
            lead += 1
        if lead:
            out.append(Gap(lo, lo + lead, "int3" if lead >= 16 else "align"))
        s, body = lo + lead, b[lead:]
        if not body:
            continue
        dwords = [struct.unpack_from("<I", body, i)[0] for i in range(0, min(len(body), 12) - 3, 4)]
        if len(dwords) >= 3 and all(tva <= d < tend for d in dwords):
            kind = "data"
        elif s in branch_targets or s in data_ptrs or body.startswith(PROLOGUES):
            kind = "code"
        else:
            head = body[:64]
            ok, pos = 0, 0
            while pos < len(head):
                ins = next(md.disasm(head[pos:], s + pos, 1), None)
                if ins is None:
                    pos += 1
                else:
                    ok += ins.size
                    pos += ins.size
            kind = "data" if (len(head) - ok) / len(head) > 0.3 else "unknown"
        out.append(Gap(s, hi, kind))
    return out


def code_seeds(gaps: list[Gap]) -> list[int]:
    return sorted(g.lo for g in gaps if g.kind == "code")
