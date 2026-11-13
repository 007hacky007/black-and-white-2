import json

import pefile
import pytest

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


def test_compact_catalog_reads_like_full(tmp_path):
    from bw2.discover import compact_catalog
    exe = _exe(tmp_path)
    full = _catalog(tmp_path, [(0x401000, 0x401006), (0x401004, 0x40100c)],
                    {"rtti": 3, "vtable": 5, "extra": 0})
    small = compact_catalog(full, tmp_path / "small.json", sha256="ab" * 32)
    assert coverage(small, exe) == coverage(full, exe)
    assert opcode_mix(small, exe) == opcode_mix(full, exe)


def test_committed_catalog_loads():
    from bw2 import paths
    from bw2.discover import load_functions
    p = paths.REPO / "bw2" / "catalog" / "white-1.42.functions.json"
    fs = load_functions(p)
    assert len(fs) == json.loads(p.read_text())["stats"]["functions"]
    assert set(fs[0]) >= {"address", "end", "entry_kind", "is_thunk"}


def test_reloc_data_targets_only_from_data_sections(tmp_path):
    import struct
    from bw2.discover import reloc_data_targets
    from fixtures import CODE, DATA, make_pe
    raw = make_pe([(".text", b"\xc3" * 32, CODE), (".data", struct.pack("<II", 0x401010, 0x12345678), DATA)],
                  {}, 0x1000)
    # add a base-relocation block by hand: one HIGHLOW at .data+0 and one at .text+4
    import pefile
    p = tmp_path / "r.dll"
    p.write_bytes(raw)
    pe = pefile.PE(str(p))
    assert reloc_data_targets(p) == []          # no reloc table yet
    from bw2.discover import _reloc_targets_from
    assert _reloc_targets_from(pe, [(0x2000, 3), (0x1004, 3)]) == [0x401010]


def _reloc_block(page: int, offsets: list[int]) -> bytes:
    import struct
    words = [0x3000 | o for o in offsets]
    if len(words) % 2:
        words.append(0)
    body = b"".join(struct.pack("<H", w) for w in words)
    return struct.pack("<II", page, 8 + len(body)) + body


def test_reloc_code_targets(tmp_path):
    # .text keeps its own pointer tables, as d3dx9_25.dll does (read-only data merged into
    # .text). Only relocated dwords outside decoded instructions and recognized jump tables
    # that point at plausible code are seeds.
    import struct
    from bw2.discover import reloc_code_targets
    text = bytearray(b"\xcc" * 0x300)

    def put(va, raw):
        text[va - 0x401000:va - 0x401000 + len(raw)] = raw

    put(0x401000, bytes.fromhex("558bec"          # push ebp; mov ebp, esp
                                "8b4508"          # mov eax, [ebp+8]
                                "83f801"          # cmp eax, 1
                                "7725"            # ja 0x401030
                                "ff2485") + struct.pack("<I", 0x401200))   # jmp [eax*4+0x401200]
    put(0x401020, b"\xb8" + struct.pack("<I", 0x401080) + b"\xeb\x09")   # mov eax, 0x401080; jmp
    put(0x401028, bytes.fromhex("33c0eb04"))                             # xor eax, eax; jmp
    put(0x401030, bytes.fromhex("5dc3"))                                 # pop ebp; ret
    put(0x401040, bytes.fromhex("33c0c3"))                               # function B
    put(0x401060, bytes.fromhex("b801000000c3"))                         # function C
    put(0x401080, bytes.fromhex("40c3"))                                 # function D: only the mov names it
    put(0x401100, struct.pack("<III", 0x401000, 0x401040, 0x401060))     # pointer table
    put(0x401140, struct.pack("<I", 0x401280))                           # pointer to zeroed data
    put(0x401200, struct.pack("<II", 0x401020, 0x401028) + bytes(8))     # the switch's jump table
    put(0x401280, bytes(32))
    reloc = _reloc_block(0x1000, [0x00E, 0x021, 0x100, 0x104, 0x108, 0x140, 0x200, 0x204])
    p = tmp_path / "r.dll"
    p.write_bytes(make_pe([(".text", bytes(text), CODE), (".reloc", reloc, DATA)], {}, 0x1000,
                          data_dirs={5: (0x2000, len(reloc))}))
    assert reloc_code_targets(p, [0x401000]) == [0x401000, 0x401040, 0x401060]
    # a vtable-scan method is taken without the decode probe
    assert reloc_code_targets(p, [0x401000], vtable={0x401280}) == [0x401000, 0x401040, 0x401060,
                                                                   0x401280]


def _negated_text() -> bytes:
    """tests/test_lifter_patches.py's negated table: `neg ecx; jmp [ecx*4 + 0x401060]` at
    0x401000, arms 0x401020/30/40 named by the slots 0x401054-0x40105C before the table."""
    import struct
    text = bytearray(b"\xcc" * 0x100)
    text[0:9] = b"\xf7\xd9\xff\x24\x8d" + struct.pack("<I", 0x401060)
    for k, arm in enumerate((0x20, 0x30, 0x40)):
        text[arm:arm + 6] = b"\xb8" + struct.pack("<I", k + 1) + b"\xc3"
    text[0x54:0x60] = struct.pack("<III", 0x401020, 0x401030, 0x401040)
    text[0x60:0x64] = bytes(4)
    return bytes(text)


def test_negated_table_marks(tmp_path):
    from bw2.discover import _decoded_marks, _pcr_disassembler, reloc_code_targets
    reloc = _reloc_block(0x1000, [0x054, 0x058, 0x05C])
    p = tmp_path / "neg.dll"
    p.write_bytes(make_pe([(".text", _negated_text(), CODE), (".reloc", reloc, DATA)], {}, 0x1000,
                          data_dirs={5: (0x2000, len(reloc))}))
    dis, _ = _pcr_disassembler(p)
    lo, hi = 0x401000, 0x401100
    cov, ist = _decoded_marks(dis, [0x401000], lo, hi)
    assert all(cov[a - lo] for a in range(0x401054, 0x401060))
    assert all(ist[a - lo] for a in (0x401020, 0x401030, 0x401040))
    # the slots are the table's, so their arms are not seeded as functions of their own
    assert reloc_code_targets(p, [0x401000]) == []


@pytest.mark.gamefiles
def test_white_negated_table_marks():
    from bw2 import paths
    from bw2.discover import _decoded_marks, _pcr_disassembler, reloc_code_targets
    exe = paths.GAMEFILES / "exe" / "white.exe"
    # white.exe's relocations all lie outside .text, so this holds by the sites alone;
    # the marks below are what would keep the slots out if any lay inside
    assert [a for a in reloc_code_targets(exe, [0xAD43D0]) if 0xAD466C <= a <= 0xAD469C] == []
    dis, _ = _pcr_disassembler(exe)
    lo, hi = 0xAD0000, 0xAE0000
    cov, ist = _decoded_marks(dis, [0xAD43D0], lo, hi)
    assert all(cov[a - lo] for a in range(0xAD464C, 0xAD466C))   # slots -7 to 0
    assert all(ist[a - lo] for a in range(0xAD466C, 0xAD46A0, 8))
