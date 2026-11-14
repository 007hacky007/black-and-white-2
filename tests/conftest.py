import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def pytest_collection_modifyitems(config, items):
    gamefiles = os.environ.get("BW2_GAMEFILES") == "1"
    long = os.environ.get("BW2_LONG") == "1"
    skip_files = pytest.mark.skip(reason="needs real game files: set BW2_GAMEFILES=1")
    skip_long = pytest.mark.skip(reason="a long test: set BW2_LONG=1")
    for item in items:
        if "gamefiles" in item.keywords and not gamefiles:
            item.add_marker(skip_files)
        elif "long" in item.keywords and not long:
            item.add_marker(skip_long)


REPO = Path(__file__).resolve().parents[1]
_PCR_TOOLS = REPO / "third_party" / "pcrecomp" / "tools"


@pytest.fixture
def lifted_c(tmp_path):
    """lifted_c(pe_bytes, va) -> C text of the function at va, lifted by pcrecomp."""
    for sub in ("pe", "disasm", "lift"):
        p = str(_PCR_TOOLS / sub)
        if p not in sys.path:
            sys.path.insert(0, p)
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from lift32 import Lifter

    def lift(pe_bytes: bytes, va: int) -> str:
        exe = tmp_path / f"lift_{va:x}.exe"
        exe.write_bytes(pe_bytes)
        info = analyze_pe(str(exe))
        iat = build_iat_map(info)
        dis = Disassembler(pe_bytes, info.image_base, info.sections)
        return Lifter(iat_map=iat, func_names={}).lift_function(dis.disassemble_function(va, iat))
    return lift
