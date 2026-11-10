import json

import pefile

from bw2.discover import code_view, coverage, opcode_mix
from fixtures import CODE, DATA, make_pe


def _exe(tmp_path, text=b"\xd9\xe8\x0f\x58\xc1\xc3" + b"\x90" * 10):
    p = tmp_path / "x.exe"
    p.write_bytes(make_pe([(".text", text, CODE), (".data", b"\0" * 16, DATA),
                           ("stxt774", b"\xc3" * 8, CODE)], {}, 0x1000))
    return p


def _catalog(tmp_path, funcs, seeds=None):
    doc = {"image_base": 0x400000, "functions": [
        {"address": a, "address_hex": f"0x{a:08X}", "name": f"sub_{a:X}", "end": e,
         "size": e - a, "num_blocks": 1, "num_instructions": 1, "is_thunk": False,
         "entry_kind": "call", "calls_to": []} for a, e in funcs]}
    if seeds:
        doc["bw2_seeds"] = seeds
    p = tmp_path / "cat.json"
    p.write_text(json.dumps(doc))
    return p


def test_coverage_unions_overlaps(tmp_path):
    exe = _exe(tmp_path)
    cat = _catalog(tmp_path, [(0x401000, 0x401008), (0x401004, 0x40100c),
                              (0x402000, 0x402004)], {"rtti": 3, "vtable": 5, "extra": 0})
    c = coverage(cat, exe)
    assert c.text_bytes == 16
    assert c.covered_bytes == 12
    assert c.functions == 3
    assert (c.seeds_rtti, c.seeds_vtable) == (3, 5)


def test_opcode_mix_counts_x87(tmp_path):
    exe = _exe(tmp_path)
    mix = opcode_mix(_catalog(tmp_path, [(0x401000, 0x401006)]), exe)
    assert mix["x87"] == 1
    assert mix["sse"] == 1
    assert mix["other"] == 1


def test_code_view_marks_only_chosen_sections_as_code(tmp_path):
    view = code_view(_exe(tmp_path), [".text"], tmp_path / "view.exe")
    pe = pefile.PE(str(view))
    flags = {s.Name.split(b"\0")[0].decode(): s.Characteristics for s in pe.sections}
    assert flags[".text"] & 0x20000020 == 0x20000020
    assert flags["stxt774"] & 0x20000020 == 0


def test_seed_list_reads_pcrecomp_and_plain_formats(tmp_path):
    from bw2.discover import _seed_list
    p = tmp_path / "s.json"
    p.write_text(json.dumps([{"address": 4198528, "address_hex": "0x00401080", "note": "x"},
                             "0x401090", 4198576]))
    assert _seed_list(p) == [0x401080, 0x401090, 0x4010B0]
