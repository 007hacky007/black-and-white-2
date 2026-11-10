"""Lift catalogued functions to sharded C with pcrecomp's lift32.Lifter."""
import json
import re
import sys
import time
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path

from .discover import code_view, load_functions
from .pcr import PCR

for _sub in ("pe", "disasm", "lift"):
    _p = str(PCR / "tools" / _sub)
    if _p not in sys.path:
        sys.path.insert(0, _p)

SYM = re.compile(r"\bsub_[0-9A-F]{8}\b")
TRAP = re.compile(r'RECOMP_TRAP\(0x[0-9A-F]{8}, "([^"]+)"\)')
HEADER = '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'


@dataclass
class ModuleLift:
    name: str
    functions: int
    shards: int
    c_bytes: int
    traps: dict
    defined: list
    referenced: list
    seconds: float


def _tools(exe: Path):
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from lift32 import Lifter
    info = analyze_pe(str(exe))
    iat = build_iat_map(info)
    dis = Disassembler(Path(exe).read_bytes(), info.image_base, info.sections)
    return dis, Lifter(iat_map=iat, func_names={}), iat


def _write(path: Path, text: str) -> None:
    part = path.with_name(path.name + ".part")
    part.write_text(text)
    part.rename(path)


def lift_module(name: str, exe: Path, catalog: Path, out_dir: Path,
                code_sections: tuple = (".text",), shard_size: int = 500) -> ModuleLift:
    t0 = time.time()
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in [*out_dir.glob(f"{name}_*.c"), *out_dir.glob(f"{name}*.part")]:
        stale.unlink()
    view = code_view(exe, list(code_sections), out_dir / f".{name}.view.exe")
    dis, lifter, iat = _tools(view)
    starts = sorted({f["address"] for f in load_functions(catalog) if f.get("entry_kind") != "alias"})
    defined, referenced, traps = [], set(), Counter()
    shards, c_bytes, chunk = 0, 0, []

    def flush():
        nonlocal shards, c_bytes, chunk
        text = HEADER + "\n".join(chunk)
        _write(out_dir / f"{name}_{shards:04d}.c", text)
        c_bytes += len(text.encode())
        shards += 1
        chunk = []

    for va in starts:
        f = dis.disassemble_function(va, iat)
        if f is None:
            raise RuntimeError(f"{name}: cannot disassemble catalogued function 0x{va:08X}")
        code = lifter.lift_function(f)
        sym = f"sub_{va:08X}"
        defined.append(sym)
        referenced.update(s for s in SYM.findall(code) if s != sym)
        traps.update(TRAP.findall(code))
        chunk.append(code)
        if len(chunk) >= shard_size:
            flush()
    if chunk:
        flush()
    m = ModuleLift(name, len(defined), shards, c_bytes, dict(sorted(traps.items())),
                   defined, sorted(referenced), round(time.time() - t0, 1))
    doc = {k: v for k, v in asdict(m).items() if k != "seconds"}   # reproducible
    _write(out_dir / f"{name}.json", json.dumps(doc, indent=0, sort_keys=True))
    return m


def lift_one(exe: Path, va: int, code_sections: tuple = (".text",)) -> str:
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        view = code_view(exe, list(code_sections), Path(td) / "view.exe")
        dis, lifter, iat = _tools(view)
        return lifter.lift_function(dis.disassemble_function(va, iat))


def link_tables(out_dir: Path, modules: list, entry: int) -> dict:
    """One header, one dispatch table and loud stubs for targets nothing lifted."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    defined = sorted({s for m in modules for s in m.defined})
    dset = set(defined)
    unresolved = sorted({s for m in modules for s in m.referenced} - dset)
    va = lambda s: int(s[4:], 16)
    hdr = ["#pragma once", "#include <stdint.h>", "",
           "void recomp_unresolved_call(uint32_t va);", "extern const uint32_t bw2_entry_va;", ""]
    hdr += [f"void {s}(void);" for s in sorted(defined + unresolved, key=va)]
    _write(out_dir / "recomp_funcs.h", "\n".join(hdr) + "\n")
    disp = ['#include "recomp_types.h"', '#include "recomp_funcs.h"', "",
            "const recomp_dispatch_entry_t recomp_dispatch_table[] = {"]
    disp += [f"    {{ 0x{va(s):08X}u, {s} }}," for s in sorted(defined, key=va)]
    disp += ["};", f"const uint32_t recomp_dispatch_count = {len(defined)};",
             f"const uint32_t bw2_entry_va = 0x{entry:08X}u;", ""]
    _write(out_dir / "recomp_dispatch.c", "\n".join(disp))
    stubs = ['#include <stdint.h>', '#include "recomp_funcs.h"', ""]
    stubs += [f"void {s}(void) {{ recomp_unresolved_call(0x{va(s):08X}u); }}"
              for s in sorted(unresolved, key=va)]
    _write(out_dir / "recomp_unresolved.c", "\n".join(stubs) + "\n")
    result = {"functions": len(defined), "unresolved": unresolved,
              "modules": sorted(m.name for m in modules)}
    _write(out_dir / "link.json", json.dumps(result, indent=0, sort_keys=True))
    return result
