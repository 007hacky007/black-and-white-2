import struct

import pytest

from bw2 import paths
from bw2.pe_survey import iat_callsites, iat_slot, survey
from fixtures import CODE, DATA, make_pe

IMP = {"d3d9.dll": ["Direct3DCreate9"]}


def _pe(tmp_path, text, extra=(), imports=IMP, name="t.exe"):
    p = tmp_path / name
    p.write_bytes(make_pe([(".text", text, CODE), *extra], imports, 0x1000))
    return p


def test_survey_synthetic(tmp_path):
    p = _pe(tmp_path, b"\xc3" * 16, [("stxt774", b"\x90" * 8, CODE)])
    s = survey(p)
    assert [x.name for x in s.sections] == [".text", "stxt774", ".idata"]
    assert s.entry == 0x401000
    assert s.imports == {"d3d9.dll": ["Direct3DCreate9"]}
    assert s.reloc_entries == 0
    assert s.tls_callbacks == []


def test_iat_callsites_synthetic(tmp_path):
    probe = _pe(tmp_path, b"\x90" * 20, name="probe.exe")
    slot = iat_slot(probe, "D3D9.DLL", "Direct3DCreate9")
    assert slot is not None
    s = struct.pack("<I", slot)
    text = b"\xff\x15" + s + b"\x90" + b"\xff\x15" + s + b"\x90" + b"\xff\x25" + s
    assert len(text) == 20
    p = _pe(tmp_path, text)
    assert iat_callsites(p, "d3d9.dll", "Direct3DCreate9") == [0x401000, 0x401007, 0x40100e]
    assert iat_callsites(p, "d3d9.dll", "Nope") == []


def test_seh_and_rtti_counts(tmp_path):
    text = b"\x64\xa1\x00\x00\x00\x00" * 3 + b"\xc3"
    rdata = b".?AVCreature@@\0" + b".?AUPoint@@\0" + b".?AVVillager@@\0"
    s = survey(_pe(tmp_path, text, [(".rdata", rdata, DATA)],
                   imports={"kernel32.dll": ["RaiseException"]}))
    assert s.seh_prologues == 3
    assert s.rtti_type_descriptors == 3
    assert s.raise_exception_imported


@pytest.mark.gamefiles
def test_real_white_exe_matches_spec():
    s = survey(paths.GAMEFILES / "exe" / "white.exe")
    assert s.entry == 0xAD8FA3
    text = next(x for x in s.sections if x.name == ".text")
    assert text.vsize == 8454603
    assert len(s.sections) == 9
    assert s.reloc_entries == 260
    assert s.reloc_pages == [0x202A000]
    assert len(s.imports["d3dx9_25.dll"]) == 60
    assert "D3DXCompileShader" in s.imports["d3dx9_25.dll"]
