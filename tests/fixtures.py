"""Builders for synthetic test inputs. None of them touch game files."""
import subprocess
import zipfile
from pathlib import Path

STEM = "Black & White 2 (USA) (Disc 1)"


def make_iso(tmp: Path, files: dict[str, bytes]) -> Path:
    src = tmp / "isosrc"
    for rel, data in files.items():
        p = src / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    iso = tmp / f"{STEM}.iso"
    subprocess.run(["hdiutil", "makehybrid", "-quiet", "-iso", "-o", str(iso), str(src)],
                   check=True)
    return iso


def iso_to_bincue(iso: Path, stem: str = STEM, mode: str = "MODE1/2352") -> Path:
    d = iso.parent / "bincue"
    d.mkdir(exist_ok=True)
    data = iso.read_bytes()
    with open(d / f"{stem}.bin", "wb") as f:
        for i in range(0, len(data), 2048):
            f.write(b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00" + b"\x00\x02\x00\x01")
            f.write(data[i:i + 2048].ljust(2048, b"\x00"))
            f.write(b"\x00" * 288)
    cue = d / f"{stem}.cue"
    cue.write_text(f'FILE "{stem}.bin" BINARY\n  TRACK 01 {mode}\n    INDEX 01 00:00:00\n')
    return cue


def zip_bincue(cue: Path) -> Path:
    z = cue.parent.parent / f"{cue.stem}.zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(cue.with_suffix(".bin"), cue.with_suffix(".bin").name)
        zf.write(cue, cue.name)
    return z


# ---- minimal PE32 builder -------------------------------------------------
import struct as _st

IMAGE_BASE, SECT_ALIGN, FILE_ALIGN = 0x400000, 0x1000, 0x200
CODE = 0x60000020   # code | execute | read
DATA = 0xC0000040   # initialised data | read | write


def _align(n, a):
    return (n + a - 1) // a * a


def _idata(imports: dict[str, list[str]], rva: int) -> bytes:
    """Import directory, ILTs, IATs, hint/names and DLL names, laid out at `rva`."""
    dlls = list(imports)
    desc_size = 20 * (len(dlls) + 1)
    thunks = sum(len(f) + 1 for f in imports.values()) * 4
    ilt_off, iat_off = desc_size, desc_size + thunks
    str_off = iat_off + thunks
    strings, names = bytearray(), {}
    for dll, funcs in imports.items():
        for fn in funcs:
            names[(dll, fn)] = str_off + len(strings)
            strings += b"\0\0" + fn.encode() + b"\0"
            if len(strings) % 2:
                strings += b"\0"
        names[dll] = str_off + len(strings)
        strings += dll.encode() + b"\0"
    desc, ilt = bytearray(), bytearray()
    k = 0
    for dll, funcs in imports.items():
        desc += _st.pack("<IIIII", rva + ilt_off + k * 4, 0, 0, rva + names[dll],
                         rva + iat_off + k * 4)
        for fn in funcs:
            ilt += _st.pack("<I", rva + names[(dll, fn)])
        ilt += b"\0\0\0\0"
        k += len(funcs) + 1
    desc += b"\0" * 20
    return bytes(desc + ilt + ilt + strings)


def make_pe(sections: list[tuple[str, bytes, int]], imports: dict[str, list[str]],
            entry_rva: int) -> bytes:
    secs = list(sections)
    rva = SECT_ALIGN
    layout = []
    for name, data, flags in secs:
        layout.append([name, data, flags, rva])
        rva += _align(max(len(data), 1), SECT_ALIGN)
    imp_rva = imp_size = 0
    if imports:
        imp_rva = rva
        data = _idata(imports, rva)
        imp_size = 20 * (len(imports) + 1)
        layout.append([".idata", data, DATA, rva])
        rva += _align(len(data), SECT_ALIGN)
    size_of_image = rva
    headers = 0x400
    raw = headers
    for s in layout:
        s.append(raw)
        raw += _align(len(s[1]), FILE_ALIGN)
    out = bytearray(raw)
    out[0:2] = b"MZ"
    _st.pack_into("<I", out, 0x3C, 0x40)
    out[0x40:0x44] = b"PE\0\0"
    _st.pack_into("<HHIIIHH", out, 0x44, 0x14C, len(layout), 0, 0, 0, 224, 0x010F)
    opt = 0x58
    dirs = [(0, 0)] * 16
    dirs[1] = (imp_rva, imp_size)
    _st.pack_into("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", out, opt,
                  0x10B, 7, 10, 0, 0, 0, entry_rva, SECT_ALIGN, 0, IMAGE_BASE,
                  SECT_ALIGN, FILE_ALIGN, 4, 0, 0, 0, 4, 0, 0, size_of_image, headers,
                  0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    for i, (r, s) in enumerate(dirs):
        _st.pack_into("<II", out, opt + 96 + i * 8, r, s)
    sh = opt + 224
    for i, (name, data, flags, srva, sraw) in enumerate(layout):
        _st.pack_into("<8sIIIIIIHHI", out, sh + i * 40, name.encode()[:8], len(data), srva,
                      _align(len(data), FILE_ALIGN), sraw, 0, 0, 0, 0, flags)
        out[sraw:sraw + len(data)] = data
    return bytes(out)


def asm32(src: str) -> bytes:
    """Intel-syntax 32-bit x86 assembled by Xcode clang; returns the .text bytes."""
    import tempfile
    from bw2.build import check_tools
    clang = check_tools()
    with tempfile.TemporaryDirectory() as td:
        s, o = Path(td) / "a.s", Path(td) / "a.o"
        s.write_text(".intel_syntax noprefix\n.text\n" + src + "\n")
        subprocess.run([str(clang), "-target", "i386-unknown-linux-elf", "-c", str(s), "-o", str(o)],
                       check=True, capture_output=True)
        d = o.read_bytes()
    shoff, = _st.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = _st.unpack_from("<HHH", d, 0x2E)
    secs = [_st.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    for s in secs:
        name = d[strtab + s[0]:d.index(b"\0", strtab + s[0])]
        if name == b".text":
            return d[s[4]:s[4] + s[5]]
    raise ValueError("no .text in assembler output")
