"""d3dx9_25.dll from disc 1: extracted, checked and rebased to 0x08000000."""
import hashlib
import struct
from pathlib import Path

import pefile
import pytest

from bw2 import paths
from fixtures import DATA, make_iso, make_pe

TEXT = struct.pack("<III", 0x00401010, 0x00402000, 0x12345678)
OPT = 0x58                    # make_pe's OptionalHeader offset
IMAGE_BASE_OFF = OPT + 28


def _reloc(entries: list[int], page: int = 0x1000) -> bytes:
    body = b"".join(struct.pack("<H", e) for e in entries)
    return struct.pack("<II", page, 8 + len(body)) + body


def _pe(entries: list[int]) -> bytes:
    reloc = _reloc(entries)
    return make_pe([(".text", TEXT, 0x60000020), (".reloc", reloc, DATA)], {}, 0x1000,
                   data_dirs={5: (0x2000, len(reloc))})


def _text_off(img: bytes) -> int:
    return pefile.PE(data=img).get_offset_from_rva(0x1000)


def test_rebase_highlow(tmp_path):
    from bw2.d3dx import rebase
    src = tmp_path / "in.dll"
    src.write_bytes(_pe([0x3000, 0x3004, 0x0000]))
    out = rebase(src, tmp_path / "out.dll", base=0x08000000)
    a, b = src.read_bytes(), out.read_bytes()
    t = _text_off(a)
    assert struct.unpack_from("<III", b, t) == (0x08001010, 0x08002000, 0x12345678)
    assert struct.unpack_from("<I", b, IMAGE_BASE_OFF)[0] == 0x08000000
    assert pefile.PE(data=b).OPTIONAL_HEADER.ImageBase == 0x08000000
    changed = set(range(t, t + 8)) | set(range(IMAGE_BASE_OFF, IMAGE_BASE_OFF + 4))
    assert len(a) == len(b)
    assert [i for i in range(len(a)) if a[i] != b[i] and i not in changed] == []
    again = rebase(src, tmp_path / "again.dll", base=0x08000000)
    assert again.read_bytes() == b


def test_rebase_rejects_dir64(tmp_path):
    from bw2.d3dx import rebase
    src = tmp_path / "in.dll"
    src.write_bytes(_pe([0x3000, 0xA008]))
    with pytest.raises(ValueError, match="relocation type 10 at RVA 0x1008"):
        rebase(src, tmp_path / "out.dll", base=0x08000000)
    assert not (tmp_path / "out.dll").exists()


def _iso_with_cab(tmp: Path, data: bytes) -> Path:
    # hdiutil drops '~' from ISO 9660 names, so the record is made under a
    # same-length name and renamed in place.
    iso = make_iso(tmp, {"DIRECTX9/APR200X2.CAB": data})
    img = iso.read_bytes()
    assert img.count(b"APR200X2.CAB") == 1
    iso.write_bytes(img.replace(b"APR200X2.CAB", b"APR200~2.CAB"))
    return iso


def test_extract_rejects_bad_hash(tmp_path):
    from bw2.d3dx import extract_dll
    iso = _iso_with_cab(tmp_path, bytes(16))
    dest = tmp_path / "d3dx"
    with pytest.raises(ValueError) as e:
        extract_dll(iso, dest)
    assert "APR200~2.CAB" in str(e.value)
    assert "16" in str(e.value)
    assert not (dest / "d3dx9_25.dll").exists()
    assert list(dest.iterdir()) == []          # the temporary folder is gone


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


@pytest.mark.gamefiles
def test_disc_dll(tmp_path):
    from bw2 import d3dx
    g = tmp_path / "gamefiles"
    g.mkdir()
    (g / "discs").symlink_to(paths.GAMEFILES / "discs")
    out = d3dx.d3dx_dll(g)
    assert out == g / "d3dx" / "d3dx9_25.0x08000000.dll"
    assert _sha(out) == d3dx.REBASED_SHA256
    orig = g / "d3dx" / d3dx.DLL_NAME
    assert orig.stat().st_size == d3dx.DLL_SIZE
    assert _sha(orig) == d3dx.DLL_SHA256

    a, b = orig.read_bytes(), out.read_bytes()
    pe = pefile.PE(data=a)
    assert pefile.PE(data=b, fast_load=True).OPTIONAL_HEADER.ImageBase == 0x08000000
    sites = []
    for block in pe.DIRECTORY_ENTRY_BASERELOC:
        for ent in block.entries:
            assert ent.type in (0, 3)
            if ent.type == 3:
                sites.append(pe.get_offset_from_rva(ent.rva))
    assert sites
    delta = (0x08000000 - pe.OPTIONAL_HEADER.ImageBase) & 0xFFFFFFFF
    assert delta == 0x07C00000
    for off in sites:
        old, new = struct.unpack_from("<I", a, off)[0], struct.unpack_from("<I", b, off)[0]
        assert (new - old) & 0xFFFFFFFF == delta, hex(off)
    ib = pe.DOS_HEADER.e_lfanew + 24 + 28
    changed = set(ib + k for k in range(4))
    for off in sites:
        changed.update(range(off, off + 4))
    assert len(a) == len(b)
    assert [i for i in range(len(a)) if a[i] != b[i] and i not in changed] == []

    mtime = out.stat().st_mtime_ns
    assert d3dx.d3dx_dll(g) == out
    assert out.stat().st_mtime_ns == mtime


EXPORTS = {"D3DXCompileShader": 0x080EB93A, "D3DXCompileShaderFromFileA": 0x080EB45C,
           "D3DXCreateEffectCompiler": 0x080F910A, "D3DXCreateFragmentLinker": 0x0816909B,
           "D3DXCreateSphere": 0x080CC4B8}


@pytest.mark.gamefiles
def test_d3dx_catalog():
    import json
    from bw2 import d3dx
    from bw2.discover import load_functions
    cat = paths.REPO / "bw2" / "catalog" / "d3dx9_25-apr2005.functions.json"
    doc = json.loads(cat.read_text())
    assert doc["sha256"] == d3dx.REBASED_SHA256
    assert doc["image_base"] == 0x08000000
    starts = {f["address"] for f in load_functions(cat) if f["entry_kind"] != "alias"}
    pe = pefile.PE(str(d3dx.d3dx_dll()))
    base = pe.OPTIONAL_HEADER.ImageBase
    by_name = {s.name.decode(): base + s.address for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
    for name, va in EXPORTS.items():
        assert by_name[name] == va, name
        assert va in starts, name
    exports = d3dx.export_seeds(d3dx.d3dx_dll())
    assert len(exports) > 300
    assert set(EXPORTS.values()) <= set(exports)
    assert [hex(a) for a in exports if a not in starts] == []
