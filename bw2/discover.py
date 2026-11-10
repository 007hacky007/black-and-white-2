"""Function discovery on white.exe without relocations, using pcrecomp's tools.

pcrecomp's analyzer treats the span from the first to the last executable
section as code. In white.exe that span runs from .text through .name0 and
takes in .rdata and the 23 MB .data, so discovery runs on a *view*: a copy of
the exe in which only the chosen sections are marked executable.
"""
import collections
import json
import struct
from dataclasses import dataclass
from pathlib import Path

import capstone
import pefile
from capstone import x86

from . import pcr
from .paths import GAMEFILES

EXEC_FLAGS = 0x20000020  # IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE


def _sname(raw: bytes) -> str:
    return raw.split(b"\0")[0].decode("latin-1")


def code_view(exe: Path, keep: list[str], out: Path) -> Path:
    data = bytearray(Path(exe).read_bytes())
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", data, pe_off + 6)[0], struct.unpack_from("<H", data, pe_off + 20)[0]
    sh = pe_off + 24 + optsz
    for i in range(nsec):
        o = sh + i * 40
        if _sname(bytes(data[o:o + 8])) not in keep:
            flags = struct.unpack_from("<I", data, o + 36)[0]
            struct.pack_into("<I", data, o + 36, flags & ~EXEC_FLAGS)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(data))
    return out


def _ranges(exe: Path, names: list[str]) -> list[tuple[int, int]]:
    pe = pefile.PE(str(exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    return [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize)
            for s in pe.sections if _sname(s.Name) in names]


def _seed_list(path: Path) -> list[int]:
    if not path.exists():
        return []
    out = []
    for e in json.loads(path.read_text()):
        if isinstance(e, dict):
            e = e.get("address", e.get("address_hex"))
        out.append(int(e, 0) if isinstance(e, str) else int(e))
    return out


def discover(exe: Path, out: Path, extra_seeds: list[int] = [],
             code_sections: list[str] = [".text"]) -> Path:
    exe, out = Path(exe).resolve(), Path(out).resolve()
    work = GAMEFILES / "discover"
    work.mkdir(parents=True, exist_ok=True)
    s_rtti, s_vt = work / "seeds_rtti.json", work / "seeds_vtable.json"
    pcr.run("tools/cpp/rtti.py", str(exe), "--seeds", str(s_rtti))
    pcr.run("tools/cpp/vtable_scan.py", str(exe), "--seeds", str(s_vt))
    rtti, vt = _seed_list(s_rtti), _seed_list(s_vt)
    ranges = _ranges(exe, code_sections)
    inside = lambda a: any(lo <= a < hi for lo, hi in ranges)
    seeds = sorted({a for a in [*rtti, *vt, *extra_seeds] if inside(a)})
    merged = work / ("seeds_" + "_".join(s.strip(".") for s in code_sections) + ".json")
    merged.write_text(json.dumps(seeds))
    view = code_view(exe, code_sections, work / ("view_" + "_".join(s.strip(".") for s in code_sections) + ".exe"))
    log = work / (out.stem + ".log")
    r = pcr.run("tools/disasm/disasm32.py", str(view), "--seed-functions", str(merged),
                "-o", str(out), check=False)
    log.write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError(f"disasm32 failed, see {log}")
    doc = json.loads(out.read_text())
    doc["exe"] = exe.name
    doc["bw2_seeds"] = {"rtti": len(rtti), "vtable": len(vt), "extra": len(extra_seeds),
                        "used": len(seeds), "code_sections": code_sections}
    for f in doc["functions"]:
        f.pop("blocks", None)  # facts only: never instruction bytes
    out.write_text(json.dumps(doc, indent=0))
    return out


@dataclass
class Coverage:
    text_bytes: int
    covered_bytes: int
    functions: int
    seeds_rtti: int
    seeds_vtable: int


def _union(intervals):
    out = []
    for lo, hi in sorted(intervals):
        if out and lo <= out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def coverage(catalog: Path, exe: Path) -> Coverage:
    doc = json.loads(Path(catalog).read_text())
    (lo, hi), = _ranges(exe, [".text"])
    iv = [(max(f["address"], lo), min(f["end"], hi)) for f in doc["functions"]]
    covered = sum(b - a for a, b in _union([(a, b) for a, b in iv if b > a]))
    s = doc.get("bw2_seeds", {})
    return Coverage(hi - lo, covered, len(doc["functions"]), s.get("rtti", 0), s.get("vtable", 0))


_GROUPS = [(x86.X86_GRP_FPU, "x87"), (x86.X86_GRP_SSE1, "sse"), (x86.X86_GRP_SSE2, "sse2"),
           (x86.X86_GRP_SSE3, "sse2"), (x86.X86_GRP_SSSE3, "sse2"), (x86.X86_GRP_SSE41, "sse2"),
           (x86.X86_GRP_SSE42, "sse2"), (x86.X86_GRP_MMX, "mmx"), (x86.X86_GRP_3DNOW, "3dnow")]


def opcode_mix(catalog: Path, exe: Path) -> dict[str, int]:
    doc = json.loads(Path(catalog).read_text())
    pe = pefile.PE(str(exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    mix = collections.Counter({k: 0 for k in ("x87", "sse", "sse2", "mmx", "3dnow", "other")})
    mnem = collections.Counter()
    for lo, hi in _union([(f["address"], f["end"]) for f in doc["functions"] if f["end"] > f["address"]]):
        code = pe.get_data(lo - base, hi - lo)
        pos = 0
        while pos < len(code):
            got = False
            for ins in md.disasm(code[pos:], lo + pos):
                got = True
                pos = ins.address + ins.size - lo
                mnem[ins.mnemonic] += 1
                g = set(ins.groups)
                mix[next((name for gid, name in _GROUPS if gid in g), "other")] += 1
            if not got:
                pos += 1
    out = dict(mix)
    out.update({f"mn:{m}": n for m, n in mnem.most_common(20)})
    return out
