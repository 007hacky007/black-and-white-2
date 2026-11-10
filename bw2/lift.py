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
HEADER = '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'


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
