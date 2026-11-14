"""Builders for synthetic test inputs. None of them touch game files."""
import re
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


def _rsrc(resources: dict[tuple[int, int], bytes], rva: int) -> bytes:
    """A resource directory (type id -> name id -> language 0x409 -> data) laid out at `rva`."""
    types = sorted({t for t, _ in resources})
    root = _st.pack("<IIHHHH", 0, 0, 0, 0, 0, len(types))
    blobs = bytearray()
    size_root = 16 + 8 * len(types)
    type_dirs = {t: sorted(n for t2, n in resources if t2 == t) for t in types}
    off = size_root
    type_off = {}
    for t in types:
        type_off[t] = off
        off += 16 + 8 * len(type_dirs[t])
    name_off = {}
    for t in types:
        for n in type_dirs[t]:
            name_off[(t, n)] = off
            off += 16 + 8
    data_entry_off = {}
    for key in sorted(resources):
        data_entry_off[key] = off
        off += 16
    data_off = {}
    for key in sorted(resources):
        data_off[key] = off + len(blobs)
        blobs += resources[key]
        blobs += b"\0" * (-len(blobs) % 4)
    out = bytearray(root)
    for t in types:
        out += _st.pack("<II", t, 0x80000000 | type_off[t])
    for t in types:
        out += _st.pack("<IIHHHH", 0, 0, 0, 0, 0, len(type_dirs[t]))
        for n in type_dirs[t]:
            out += _st.pack("<II", n, 0x80000000 | name_off[(t, n)])
    for t in types:
        for n in type_dirs[t]:
            out += _st.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + _st.pack("<II", 0x409, data_entry_off[(t, n)])
    for key in sorted(resources):
        out += _st.pack("<IIII", rva + data_off[key], len(resources[key]), 0, 0)
    return bytes(out + blobs)


def _edata(exports: dict[str, int], rva: int, dll_name: str) -> bytes:
    """An export directory: names sorted, ordinals from 1 in that order, at `rva`."""
    names = sorted(exports)
    n = len(names)
    funcs_off = 40
    names_off = funcs_off + 4 * n
    ords_off = names_off + 4 * n
    str_off = ords_off + 2 * n
    strings = bytearray(dll_name.encode() + b"\0")
    name_rvas = []
    for nm in names:
        name_rvas.append(rva + str_off + len(strings))
        strings += nm.encode() + b"\0"
    out = bytearray(_st.pack("<IIHHIIIIIII", 0, 0, 0, 0, rva + str_off, 1, n, n,
                             rva + funcs_off, rva + names_off, rva + ords_off))
    out += b"".join(_st.pack("<I", exports[nm]) for nm in names)
    out += b"".join(_st.pack("<I", r) for r in name_rvas)
    out += b"".join(_st.pack("<H", i) for i in range(n))
    return bytes(out + strings)


def make_pe(sections: list[tuple[str, bytes, int]], imports: dict[str, list[str]],
            entry_rva: int, resources: dict[tuple[int, int], bytes] | None = None,
            data_dirs: dict[int, tuple[int, int]] | None = None, image_base: int = IMAGE_BASE,
            exports: dict[str, int] | None = None, dll_name: str = "") -> bytes:
    """A PE32 image; data_dirs sets extra directories, {index: (rva, size)}. With exports
    ({name: rva}) it is a DLL named dll_name."""
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
    rsrc_rva = rsrc_size = 0
    if resources:
        rsrc_rva = rva
        data = _rsrc(resources, rva)
        rsrc_size = len(data)
        layout.append([".rsrc", data, DATA, rva])
        rva += _align(len(data), SECT_ALIGN)
    exp_rva = exp_size = 0
    if exports:
        exp_rva = rva
        data = _edata(exports, rva, dll_name or "test.dll")
        exp_size = len(data)
        layout.append([".edata", data, DATA, rva])
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
    _st.pack_into("<HHIIIHH", out, 0x44, 0x14C, len(layout), 0, 0, 0, 224, 0x210E if exports else 0x010F)
    opt = 0x58
    dirs = [(0, 0)] * 16
    dirs[1] = (imp_rva, imp_size)
    dirs[0] = (exp_rva, exp_size)
    dirs[2] = (rsrc_rva, rsrc_size)
    for k, v in (data_dirs or {}).items():
        dirs[k] = v
    _st.pack_into("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", out, opt,
                  0x10B, 7, 10, 0, 0, 0, entry_rva, SECT_ALIGN, 0, image_base,
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


def asm32(src: str, base: int | None = None) -> bytes:
    """Intel-syntax 32-bit x86 assembled by Xcode clang; returns the .text bytes.

    With `base`, the object's R_386_32 and R_386_PC32 relocations against its own
    .text symbols (and absolute symbols) are applied as if .text sat at `base`, so
    labels can hold data such as MSVC EH tables."""
    import tempfile
    from bw2.build import check_tools
    clang = check_tools()
    with tempfile.TemporaryDirectory() as td:
        s, o = Path(td) / "a.s", Path(td) / "a.o"
        # clang's Intel parser assembles `push offset sym` as a 16-bit pushw: spell it out.
        src = re.sub(r"(?m)^(\s*)push\s+offset\s+(\S+)\s*$", r"\1.byte 0x68\n\1.long \2", src)
        s.write_text(".intel_syntax noprefix\n.text\n" + src + "\n")
        subprocess.run([str(clang), "-target", "i386-unknown-linux-elf", "-c", str(s), "-o", str(o)],
                       check=True, capture_output=True)
        d = o.read_bytes()
    shoff, = _st.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = _st.unpack_from("<HHH", d, 0x2E)
    secs = [_st.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    name = lambda sec: d[strtab + sec[0]:d.index(b"\0", strtab + sec[0])]
    text_i = next((i for i, sec in enumerate(secs) if name(sec) == b".text"), None)
    if text_i is None:
        raise ValueError("no .text in assembler output")
    text = bytearray(d[secs[text_i][4]:secs[text_i][4] + secs[text_i][5]])
    rels = [sec for sec in secs if sec[1] == 9 and sec[7] == text_i]      # SHT_REL for .text
    if rels and base is None:
        raise ValueError("assembler output has relocations; pass base=")
    for rel in rels:
        symtab = secs[rel[6]]
        for k in range(rel[5] // 8):
            off, info = _st.unpack_from("<II", d, rel[4] + 8 * k)
            sym, kind = info >> 8, info & 0xFF
            _n, value, _sz, _info, _oth, shndx = _st.unpack_from("<IIIBBH", d, symtab[4] + 16 * sym)
            if shndx == text_i:
                target = base + value
            elif shndx == 0xFFF1:                  # SHN_ABS
                target = value
            else:
                raise ValueError(f"relocation against an undefined symbol at .text+0x{off:x}")
            addend, = _st.unpack_from("<i", text, off)
            if kind == 1:                          # R_386_32: S + A
                v = target + addend
            elif kind == 2:                        # R_386_PC32: S + A - P
                v = target + addend - (base + off)
            else:
                raise ValueError(f"unsupported relocation type {kind}")
            _st.pack_into("<I", text, off, v & 0xFFFFFFFF)
    return bytes(text)


def iat_va(sections: list[tuple[str, bytes, int]], imports: dict[str, list[str]],
           dll: str, fn: str) -> int:
    """The IAT slot VA make_pe gives dll!fn, for sections of these sizes."""
    import pefile
    pe = pefile.PE(data=make_pe(sections, imports, 0x1000))
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        for i in d.imports:
            if d.dll.decode() == dll and i.name and i.name.decode() == fn:
                return i.address
    raise KeyError(f"{dll}!{fn}")


# ---- engine guests --------------------------------------------------------
def _catalog(path: Path, functions: list[int]) -> Path:
    import json
    rows = [[va, va + 1, "start", 0] for va in sorted(set(functions))]
    path.write_text(json.dumps({"columns": ["address", "end", "entry_kind", "is_thunk"], "functions": rows}))
    return path


def guest_engine(tmp_path: Path, pe: bytes, functions: list[int], exe_name: str = "game.exe",
                 dlls: dict | None = None, code_sections: tuple = (".text",),
                 computed_ret: bool = True, runtime_push=frozenset()) -> Path:
    """Lift a synthetic PE (and DLLs: {relative path: (bytes, function starts)}) and link
    it with the engine host; the files go to tmp_path/game, the binary is returned."""
    import struct
    import tempfile
    from bw2.build import build
    from bw2.lift import lift_module, link_tables
    game = tmp_path / "game"
    game.mkdir(exist_ok=True)
    (game / exe_name).write_bytes(pe)
    gen = tmp_path / "gen"
    mods = [lift_module("main", game / exe_name, _catalog(tmp_path / "main.json", functions), gen, code_sections,
                        computed_ret=computed_ret, runtime_push=runtime_push)]
    for i, (rel, (data, funcs)) in enumerate(sorted((dlls or {}).items())):
        p = game / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        mods.append(lift_module(f"dll{i}", p, _catalog(tmp_path / f"dll{i}.json", funcs), gen, (".text",)))
    pe_off, = struct.unpack_from("<I", pe, 0x3C)
    entry = struct.unpack_from("<I", pe, pe_off + 0x28)[0] + struct.unpack_from("<I", pe, pe_off + 0x34)[0]
    link_tables(gen, mods, entry=entry)
    cache = Path(tempfile.gettempdir()) / "bw2-test-engine-host"
    return build(gen, tmp_path / "bw2", opt="-O1", host="engine", host_obj_dir=cache).binary


def run_engine(binary: Path, game_dir: Path, support_dir: Path, *args: str, exe: str = "game.exe",
               timeout: int = 60) -> subprocess.CompletedProcess:
    import os
    env = dict(os.environ, BW2_SUPPORT=str(support_dir))
    return subprocess.run([str(binary), "--game", str(game_dir), "--exe", exe, *args],
                          capture_output=True, text=True, timeout=timeout, env=env)


# ---- app bundles (plan 13 Task 4) -----------------------------------------
_REPO = Path(__file__).resolve().parents[1]
APP_NAME = "Black & White 2.app"


def _clone_file(src: Path, dst: Path) -> None:
    import shutil
    from bw2.gametree import _clonefile
    if not _clonefile(src, dst):
        shutil.copy2(src, dst)


def make_test_bundle(dest: Path, game: Path | None, name: str = APP_NAME,
                     engine: Path = _REPO / "out" / "bw2",
                     dll: Path | None = _REPO / "out" / "d3dx9_25.dll") -> Path:
    """An unsigned app bundle dest/name: Contents/MacOS/bw2 (a copy of engine), Contents/Resources/
    d3dx9_25.dll (a copy of dll; none for dll=None), Contents/Resources/game (an APFS clone of game, or an
    empty folder for None) and a minimal Info.plist. Returns the bundle's path."""
    import plistlib
    from bw2.gametree import clone_tree
    app = Path(dest) / name
    macos, res = app / "Contents" / "MacOS", app / "Contents" / "Resources"
    macos.mkdir(parents=True)
    res.mkdir()
    assert Path(engine).exists(), f"no engine at {engine}: build it first (python -m bw2.pipeline)"
    _clone_file(Path(engine), macos / "bw2")
    if dll is not None:
        assert Path(dll).exists(), f"no lifted DLL at {dll}: build it first (python -m bw2.pipeline)"
        _clone_file(Path(dll), res / "d3dx9_25.dll")
    if game is None:
        (res / "game").mkdir()
    else:
        clone_tree(Path(game), res / "game")
    info = {"CFBundleExecutable": "bw2", "CFBundleIdentifier": "local.bw2-port.game", "CFBundleName": "Black & White 2",
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.1.0", "LSMinimumSystemVersion": "13.0",
            "NSHighResolutionCapable": True}
    (app / "Contents" / "Info.plist").write_bytes(plistlib.dumps(info))
    return app
