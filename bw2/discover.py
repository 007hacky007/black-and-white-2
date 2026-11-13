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
from .catclean import alias_cfg_labels, clean_full, seed_classes
from .gaps import code_seeds, gaps
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


def _reloc_targets_from(pe: pefile.PE, entries: list[tuple[int, int]]) -> list[int]:
    """HIGHLOW relocation sites in non-executable sections whose value points into .text."""
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if _sname(s.Name) == ".text")
    lo, hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
    data_secs = [s for s in pe.sections if not s.Characteristics & 0x20000000]
    out = set()
    for rva, typ in entries:
        if typ != 3 or not any(s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize
                                 for s in data_secs):
            continue
        v = pe.get_dword_at_rva(rva)
        if v is not None and lo <= v < hi:
            out.add(v)
    return sorted(out)


def reloc_data_targets(exe: Path) -> list[int]:
    """Exact code pointers stored in data, for images that keep their relocations (DLLs)."""
    pe = pefile.PE(str(exe))
    entries = [(e.rva, e.type) for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
               for e in blk.entries]
    return _reloc_targets_from(pe, entries) if entries else []


def _pcr_disassembler(exe: Path):
    """pcrecomp's Disassembler and IAT map for an image, as the lifter builds them."""
    from .pcr import PCR
    import sys
    for sub in ("pe", "disasm"):
        p = str(PCR / "tools" / sub)
        if p not in sys.path:
            sys.path.insert(0, p)
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    info = analyze_pe(str(exe))
    return Disassembler(Path(exe).read_bytes(), info.image_base, info.sections), build_iat_map(info)


def _d32(dis):
    """The disasm32 module `dis` came from, for its shared jump-table helpers."""
    import sys
    return sys.modules[type(dis).__module__]


def _table_span(dis, table: int, limit: int = 256, negated: bool = False) -> tuple[int, int]:
    """The slots of the jump table at `table` that disasm32's table_entries reads as
    entries, with its own end rule (a couple of non-code slots end it).

    negated (disasm32's negated_index: `neg r; jmp [r*4 + table]`): the k code slots
    before `table` that table_entries_before reads, and slot 0."""
    if negated:
        k = len(_d32(dis).table_entries_before(dis.read_bytes, table, dis.is_code_address))
        return table - 4 * k, table + 4
    last, misses = -1, 0
    for k in range(limit):
        raw = dis.read_bytes(table + 4 * k, 4)
        if not raw or len(raw) < 4:
            break
        if dis.is_code_address(int.from_bytes(raw, "little")):
            last, misses = k, 0
            continue
        misses += 1
        if (last >= 0 and misses >= 2) or (last < 0 and misses >= 4):
            break
    return table, table + 4 * (last + 1)


def _decoded_marks(dis, roots, lo: int, hi: int) -> tuple[bytearray, bytearray]:
    """(covered, starts): a byte per .text address, set where a decoded instruction or a
    jump table it dispatches through lies, and where an instruction starts.

    The union of what disasm32's disassemble_function decodes from each root (its first
    pass: straight-line scans from block leaders, following branches within 1 MB and
    jump-table arms, read backwards after `neg` of the index as disasm32 does), walked once with a shared visited set: decoding an address gives
    the same instruction whichever function reaches it, and image bodies overlap a lot
    (d3dx9_25.dll's 5,855 starts decode 12.7M instructions between them)."""
    cov, ist = bytearray(hi - lo), bytearray(hi - lo)
    negated_index = _d32(dis).negated_index
    seen, work = set(), [(r, r) for r in sorted(set(roots)) if lo <= r < hi]

    def mark(a: int, b: int) -> None:
        a, b = max(a, lo), min(b, hi)
        if b > a:
            cov[a - lo:b - lo] = b"\x01" * (b - a)

    def follow(t, root):
        if t is not None and dis.is_code_address(t) and abs(t - root) < 0x100000 and t not in seen:
            work.append((t, root))

    while work:
        addr, root = work.pop()
        if addr in seen or not dis.is_code_address(addr):
            continue
        seen.add(addr)
        last = None
        for ins in dis.disassemble_at(addr, max_bytes=8192):
            if ins.address != addr and lo <= ins.address < hi and ist[ins.address - lo]:
                last = None
                break                            # joined code already walked
            prev, last = last, ins
            mark(ins.address, ins.end_address)
            if lo <= ins.address < hi:
                ist[ins.address - lo] = 1
            if ins.is_call:
                continue
            if ins.is_cond_jump:
                follow(ins.get_branch_target(), root)
                follow(ins.end_address, root)
                break
            if ins.is_uncond_jump:
                t = ins.get_branch_target()
                if t is not None:
                    follow(t, root)
                else:
                    neg = negated_index(prev, ins)
                    arms = dis.jump_table_targets(ins, negated=neg)
                    if arms:
                        mark(*_table_span(dis, ins.operands[0].mem.disp & 0xFFFFFFFF, negated=neg))
                    for arm in arms:
                        if arm not in seen:
                            work.append((arm, root))
                break
            if ins.is_ret or ins.mnemonic == "int3":
                break
        else:
            if last is not None and last.end_address not in seen:
                work.append((last.end_address, root))
    return cov, ist


def reloc_code_targets(exe: Path, decoded, vtable=frozenset()) -> list[int]:
    """Code pointers that .text itself holds, for images that keep read-only data there.

    A HIGHLOW relocation site inside .text whose dword points into .text, when the site
    lies outside every instruction decoded from the function starts in `decoded` (a first
    disassembly round) and outside every jump table those dispatch through, so neither an
    instruction's operand nor a switch's mid-function arms count. The target must be a
    vtable-scan method (`vtable`), or not fall inside a decoded instruction and decode as
    a function body (disasm32's probes_as_function_body)."""
    pe = pefile.PE(str(exe))
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if _sname(s.Name) == ".text")
    lo, hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
    sites = sorted({base + e.rva for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
                    for e in blk.entries if e.type == 3 and lo <= base + e.rva < hi})
    if not sites:
        return []
    dis, _ = _pcr_disassembler(exe)
    cov, ist = _decoded_marks(dis, decoded, lo, hi)
    img = pe.get_memory_mapped_image()
    vtable, out = set(vtable), set()
    for site in sites:
        if any(cov[k - lo] for k in range(site, min(site + 4, hi))):
            continue
        v = struct.unpack_from("<I", img, site - base)[0]
        if not lo <= v < hi:
            continue
        if v in vtable or ((ist[v - lo] or not cov[v - lo]) and dis.probes_as_function_body(v)):
            out.add(v)
    return sorted(out)


def discover(exe: Path, out: Path, extra_seeds: tuple[int, ...] = (),
             code_sections: tuple[str, ...] = (".text",), max_rounds: int = 4,
             code_reloc_seeds: bool = False, cfg_aliases: bool = False) -> Path:
    """Run discovery; the compact catalog goes to `out`, the full one stays in GAMEFILES.

    code_reloc_seeds: after the first round, add reloc_code_targets (pointers .text holds
    itself) as strong seeds for the rounds that follow. For images that merge their
    read-only data into .text (d3dx9_25.dll); off for the others.

    cfg_aliases: after cleaning, catclean.alias_cfg_labels folds starts that only
    direct jumps reach into the trusted bodies whose control flow holds them. Sound
    only for an image that keeps its relocations (d3dx9_25.dll); off for the others."""
    exe, out = Path(exe).resolve(), Path(out).resolve()
    work = GAMEFILES / "discover"
    work.mkdir(parents=True, exist_ok=True)
    s_rtti, s_vt = work / f"{out.stem}.seeds_rtti.json", work / f"{out.stem}.seeds_vtable.json"
    pcr.run("tools/cpp/rtti.py", str(exe), "--seeds", str(s_rtti))
    pcr.run("tools/cpp/vtable_scan.py", str(exe), "--seeds", str(s_vt))
    rtti, vt = _seed_list(s_rtti), _seed_list(s_vt)
    relocs = reloc_data_targets(exe) if ".text" in code_sections else []
    extra_seeds = tuple(extra_seeds) + tuple(relocs)
    ranges = _ranges(exe, code_sections)
    inside = lambda a: any(lo <= a < hi for lo, hi in ranges)
    seeds = sorted({a for a in [*rtti, *vt, *extra_seeds] if inside(a)})
    merged = work / f"{out.stem}.seeds_used.json"
    merged.write_text(json.dumps(seeds))
    view = code_view(exe, code_sections, work / ("view_" + "_".join(s.strip(".") for s in code_sections) + ".exe"))
    log = work / (out.stem + ".log")
    full = work / (out.stem + ".full.json")
    rounds, added, code_relocs = 0, [], []
    while True:
        rounds += 1
        merged.write_text(json.dumps(sorted(set(seeds) | set(added) | set(code_relocs))))
        r = pcr.run("tools/disasm/disasm32.py", str(view), "--seed-functions", str(merged),
                    "-o", str(full), check=False)
        log.write_text(r.stdout + r.stderr)
        if r.returncode != 0:
            raise RuntimeError(f"disasm32 failed, see {log}")
        if ".text" not in code_sections or rounds >= max_rounds:
            break
        have = {f["address"] for f in load_functions(full)}
        fresh = []
        if code_reloc_seeds and rounds == 1:
            code_relocs = [a for a in reloc_code_targets(exe, sorted(have), vt) if inside(a)]
            fresh = [a for a in code_relocs if a not in have]
        new = [a for a in code_seeds(gaps(full, exe)) if a not in have and a not in added]
        if not new and not fresh:
            break
        added += new
    doc = json.loads(full.read_text())
    doc["exe"] = exe.name
    doc["bw2_seeds"] = {"rtti": len(rtti), "vtable": len(vt), "extra": len(extra_seeds),
                        "used": len(seeds), "code_sections": list(code_sections),
                        "rounds": rounds, "gap_seeds": len(added), "reloc_seeds": len(relocs)}
    if code_reloc_seeds:
        doc["bw2_seeds"]["code_reloc_seeds"] = len(code_relocs)
    full.write_text(json.dumps(doc, indent=0))
    merged.write_text(json.dumps(sorted(set(seeds) | set(added) | set(code_relocs))))
    (work / f"{out.stem}.seeds_gap.json").write_text(json.dumps(sorted(added)))
    if code_reloc_seeds:
        (work / f"{out.stem}.seeds_code_reloc.json").write_text(json.dumps(code_relocs))
    full.with_suffix(".uncleaned.json").write_text(full.read_text())
    strong, weak = seed_classes(rtti, vt, tuple(extra_seeds) + tuple(code_relocs), added)
    clean_full(full, exe, strong, weak)
    if cfg_aliases:
        alias_cfg_labels(full, view, strong)
    from .releases import sha256_file
    return compact_catalog(full, out, sha256_file(exe))


COMPACT_COLUMNS = ["address", "end", "entry_kind", "is_thunk"]


def load_functions(catalog: Path) -> list[dict]:
    """Functions from a full disasm32 catalog or from our compact one."""
    doc = json.loads(Path(catalog).read_text())
    if doc.get("columns"):
        cols = doc["columns"]
        return [dict(zip(cols, row)) for row in doc["functions"]]
    return doc["functions"]


def compact_catalog(full: Path, out: Path, sha256: str) -> Path:
    """The committed form: address, end, entry kind, thunk flag. No bytes, no names."""
    doc = json.loads(Path(full).read_text())
    small = {"exe": doc.get("exe"), "sha256": sha256, "image_base": doc.get("image_base"),
             "stats": doc.get("stats"), "bw2_seeds": doc.get("bw2_seeds"),
             "columns": COMPACT_COLUMNS,
             "functions": [[f["address"], f["end"], f["entry_kind"], int(f["is_thunk"])]
                           for f in doc["functions"]]}
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(small, separators=(",", ":")))
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
    fs = load_functions(catalog)
    (lo, hi), = _ranges(exe, [".text"])
    iv = [(max(f["address"], lo), min(f["end"], hi)) for f in fs]
    covered = sum(b - a for a, b in _union([(a, b) for a, b in iv if b > a]))
    s = doc.get("bw2_seeds") or {}
    return Coverage(hi - lo, covered, len(fs), s.get("rtti", 0), s.get("vtable", 0))


_GROUPS = [(x86.X86_GRP_FPU, "x87"), (x86.X86_GRP_SSE1, "sse"), (x86.X86_GRP_SSE2, "sse2"),
           (x86.X86_GRP_SSE3, "sse2"), (x86.X86_GRP_SSSE3, "sse2"), (x86.X86_GRP_SSE41, "sse2"),
           (x86.X86_GRP_SSE42, "sse2"), (x86.X86_GRP_MMX, "mmx"), (x86.X86_GRP_3DNOW, "3dnow")]


def opcode_mix(catalog: Path, exe: Path) -> dict[str, int]:
    fs = load_functions(catalog)
    pe = pefile.PE(str(exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    mix = collections.Counter({k: 0 for k in ("x87", "sse", "sse2", "mmx", "3dnow", "other")})
    mnem = collections.Counter()
    for lo, hi in _union([(f["address"], f["end"]) for f in fs if f["end"] > f["address"]]):
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
