"""The engine: lifted guests linked with the win32hle host and the BW2 runtime."""
import pytest

from fixtures import CODE, asm32, guest_engine, iat_va, make_pe, run_engine

BASE = 0x401000          # .text VA for every test image


def _image(asm: str, imports: dict[str, list[str]], text_len: int = 0x800):
    """Assemble `asm` at BASE; {IAT_<fn>} placeholders become the slots' addresses."""
    pad = [(".text", b"\xcc" * text_len, CODE)]
    slots = {f"IAT_{fn}": iat_va(pad, imports, dll, fn) for dll, fns in imports.items() for fn in fns}
    code = asm32(asm.format(**slots), base=BASE)
    assert len(code) <= text_len
    return make_pe([(".text", code.ljust(text_len, b"\xcc"), CODE)], imports, 0x1000)


def _engine(tmp_path, asm, imports, functions=(BASE,), **kw):
    pe = _image(asm, imports)
    return guest_engine(tmp_path, pe, list(functions), **kw)


def _run(tmp_path, binary, *args, **kw):
    return run_engine(binary, tmp_path / "game", tmp_path / "support", *args, **kw)


K32 = "KERNEL32.dll"


# ---- Task 1: the engine links and runs -------------------------------------

def test_engine_runs_lifted_entry(tmp_path):
    b = _engine(tmp_path, "push 7\ncall dword ptr [{IAT_ExitProcess}]\nret", {K32: ["ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 7, r.stderr[-2000:]


def test_unimplemented_import_named(tmp_path):
    b = _engine(tmp_path, "call dword ptr [{IAT_BogusCall}]\npush 0\ncall dword ptr [{IAT_ExitProcess}]\nret",
                {K32: ["BogusCall", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode != 0 and "BogusCall" in r.stderr, r.stderr[-2000:]


def test_strict_lists_unanswered_imports(tmp_path):
    b = _engine(tmp_path, "call dword ptr [{IAT_BogusCall}]\nret", {K32: ["BogusCall", "ExitProcess"]})
    r = _run(tmp_path, b, "--strict")
    assert r.returncode == 2 and "BogusCall" in r.stderr, r.stderr[-2000:]


def test_dispatch_is_hashed(tmp_path):
    asm = "f0: call f1\nret\nf1: call f2\nret\nf2: xor eax, eax\nret"
    pe = _image(asm, {K32: ["ExitProcess"]})
    code = asm32(asm, base=BASE)
    b = guest_engine(tmp_path, pe, [BASE, BASE + 6, BASE + 12])
    r = _run(tmp_path, b, "--selftest-dispatch")
    assert r.returncode == 0 and "dispatch ok 3" in r.stdout, r.stdout + r.stderr[-2000:]
    assert len(code) == 15


def test_guest_memory_is_based(tmp_path):
    b = _engine(tmp_path, "ret", {K32: ["ExitProcess"]})
    r = _run(tmp_path, b, "--selftest-mem")
    assert r.returncode == 0 and "mem base 0x100000000000" in r.stdout, r.stdout + r.stderr[-2000:]


# ---- Task 2: mounts, support folder, registry ------------------------------

INSTALL = r"C:\Program Files\EA GAMES\Black & White 2"
# puts: write the C string at esi and a newline to stdout. Lives at BASE + 0x400.
PUTS = r"""
.org 0x400
puts:
    mov ecx, esi
1:  cmp byte ptr [ecx], 0
    je 2f
    inc ecx
    jmp 1b
2:  sub ecx, esi
    push 0
    push offset scratch
    push ecx
    push esi
    push -11
    call dword ptr [{IAT_GetStdHandle}]
    push eax
    call dword ptr [{IAT_WriteFile}]
    push 0
    push offset scratch
    push 1
    push offset newline
    push -11
    call dword ptr [{IAT_GetStdHandle}]
    push eax
    call dword ptr [{IAT_WriteFile}]
    ret
newline: .byte 10
scratch: .long 0
"""
IO = ["GetStdHandle", "WriteFile", "ReadFile", "CreateFileA", "CloseHandle", "ExitProcess"]


def _tree(root):
    import hashlib
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def test_writes_land_in_support_dir(tmp_path):
    asm = r"""
    push 0
    push 0x80
    push 2
    push 0
    push 0
    push 0x40000000
    push offset name
    call dword ptr [{IAT_CreateFileA}]
    mov esi, eax
    push 0
    push offset count
    push 3
    push offset xyz
    push esi
    call dword ptr [{IAT_WriteFile}]
    push esi
    call dword ptr [{IAT_CloseHandle}]
    push 0
    push 0x80
    push 3
    push 0
    push 1
    push 0x80000000
    push offset name
    call dword ptr [{IAT_CreateFileA}]
    mov esi, eax
    push 0
    push offset count
    push 3
    push offset buf
    push esi
    call dword ptr [{IAT_ReadFile}]
    push esi
    call dword ptr [{IAT_CloseHandle}]
    movzx eax, byte ptr [buf + 2]
    push eax
    call dword ptr [{IAT_ExitProcess}]
name: .asciz "C:\\Program Files\\EA GAMES\\Black & White 2\\data.txt"
xyz: .ascii "xyz"
count: .long 0
buf: .long 0
"""
    b = _engine(tmp_path, asm, {K32: IO})
    before = _tree(tmp_path / "game")
    r = _run(tmp_path, b)
    assert r.returncode == ord("z"), r.stderr[-2000:]
    assert (tmp_path / "support" / "Game" / "data.txt").read_bytes() == b"xyz"
    assert _tree(tmp_path / "game") == before


def test_game_files_read_case_insensitively(tmp_path):
    asm = r"""
    push 0
    push 0x80
    push 3
    push 0
    push 1
    push 0x80000000
    push offset name
    call dword ptr [{IAT_CreateFileA}]
    mov esi, eax
    push 0
    push offset count
    push 1
    push offset buf
    push esi
    call dword ptr [{IAT_ReadFile}]
    movzx eax, byte ptr [buf]
    push eax
    call dword ptr [{IAT_ExitProcess}]
name: .asciz "DATA\\A.TXT"
count: .long 0
buf: .long 0
"""
    b = _engine(tmp_path, asm, {K32: IO})
    (tmp_path / "game" / "Data").mkdir()
    (tmp_path / "game" / "Data" / "a.txt").write_bytes(b"Q")
    r = _run(tmp_path, b)
    assert r.returncode == ord("Q"), r.stderr[-2000:]


def test_module_path_and_cwd(tmp_path):
    asm = r"""
    push 260
    push offset path
    push 0
    call dword ptr [{IAT_GetModuleFileNameA}]
    mov esi, offset path
    call puts
    push offset path
    push 260
    call dword ptr [{IAT_GetCurrentDirectoryA}]
    mov esi, offset path
    call puts
    push 0
    call dword ptr [{IAT_ExitProcess}]
path: .fill 264, 1, 0
""" + PUTS
    b = _engine(tmp_path, asm, {K32: IO + ["GetModuleFileNameA", "GetCurrentDirectoryA"]},
                functions=(BASE, BASE + 0x400), exe_name="white.exe")
    r = _run(tmp_path, b, exe="white.exe")
    assert r.returncode == 0, r.stderr[-2000:]
    lines = r.stdout.splitlines()
    assert lines[:2] == [INSTALL + r"\white.exe", INSTALL], r.stdout


def test_my_documents_path(tmp_path):
    asm = r"""
    push offset path
    push 0
    push 0
    push 5
    push 0
    call dword ptr [{IAT_SHGetFolderPathA}]
    mov esi, offset path
    call puts
    push offset path
    call dword ptr [{IAT_SetCurrentDirectoryA}]
    push 0
    push 0x80
    push 2
    push 0
    push 0
    push 0x40000000
    push offset name
    call dword ptr [{IAT_CreateFileA}]
    push eax
    call dword ptr [{IAT_CloseHandle}]
    push 0
    call dword ptr [{IAT_ExitProcess}]
name: .asciz "note.txt"
path: .fill 264, 1, 0
""" + PUTS
    b = _engine(tmp_path, asm, {K32: IO + ["SetCurrentDirectoryA"], "SHELL32.dll": ["SHGetFolderPathA"]},
                functions=(BASE, BASE + 0x400))
    r = _run(tmp_path, b)
    assert r.returncode == 0, r.stderr[-2000:]
    assert r.stdout.splitlines()[0] == r"C:\Documents and Settings\Player\My Documents", r.stdout
    assert (tmp_path / "support" / "C" / "Documents and Settings" / "Player" / "My Documents" / "note.txt").exists()


def test_registry_persists(tmp_path):
    common = r"""
key: .asciz "Software\\BW2Test"
vname: .asciz "v"
hk: .long 0
disp: .long 0
val: .long 42
size: .long 4
kind: .long 0
"""
    write = r"""
    push offset disp
    push offset hk
    push 0
    push 0xF003F
    push 0
    push 0
    push 0
    push offset key
    push 0x80000001
    call dword ptr [{IAT_RegCreateKeyExA}]
    push 4
    push offset val
    push 4
    push 0
    push offset vname
    push dword ptr [hk]
    call dword ptr [{IAT_RegSetValueExA}]
    push dword ptr [hk]
    call dword ptr [{IAT_RegCloseKey}]
    push 0
    call dword ptr [{IAT_ExitProcess}]
""" + common
    read = r"""
    mov dword ptr [val], 0
    push offset hk
    push 0x20019
    push 0
    push offset key
    push 0x80000001
    call dword ptr [{IAT_RegOpenKeyExA}]
    push offset size
    push offset val
    push offset kind
    push 0
    push offset vname
    push dword ptr [hk]
    call dword ptr [{IAT_RegQueryValueExA}]
    push dword ptr [val]
    call dword ptr [{IAT_ExitProcess}]
""" + common
    adv = ["RegCreateKeyExA", "RegSetValueExA", "RegCloseKey", "RegOpenKeyExA", "RegQueryValueExA"]
    support = tmp_path / "support"
    for name, asm in (("w", write), ("r", read)):
        d = tmp_path / name
        d.mkdir()
        b = _engine(d, asm, {K32: ["ExitProcess"], "ADVAPI32.dll": adv})
        r = run_engine(b, d / "game", support)
        assert r.returncode == (0 if name == "w" else 42), r.stderr[-2000:]
    assert (support / "registry.txt").exists()


def test_game_files_cannot_be_deleted(tmp_path):
    asm = r"""
    push offset name
    call dword ptr [{IAT_DeleteFileA}]
    push eax
    call dword ptr [{IAT_ExitProcess}]
name: .asciz "Data\\a.txt"
"""
    b = _engine(tmp_path, asm, {K32: ["DeleteFileA", "ExitProcess"]})
    (tmp_path / "game" / "Data").mkdir()
    (tmp_path / "game" / "Data" / "a.txt").write_bytes(b"Q")
    r = _run(tmp_path, b)
    assert r.returncode == 0, r.stderr[-2000:]          # DeleteFileA fails
    assert (tmp_path / "game" / "Data" / "a.txt").read_bytes() == b"Q"


# ---- Task 3: threads ---------------------------------------------------------

THR = ["CreateThread", "WaitForSingleObject", "ExitProcess"]
WORKER = BASE + 0x400


def test_spin_wait_hands_over(tmp_path):
    asm = r"""
    push 0
    push 0
    push 0
    push {worker}
    push 0
    push 0
    call dword ptr [{{IAT_CreateThread}}]
spin:
    cmp dword ptr [flag], 0
    je spin
    push 5
    call dword ptr [{{IAT_ExitProcess}}]
flag: .long 0
.org 0x400
    mov dword ptr [flag], 1
    ret 4
""".format(worker=WORKER)
    b = _engine(tmp_path, asm, {K32: THR}, functions=(BASE, WORKER))
    r = _run(tmp_path, b, timeout=10)
    assert r.returncode == 5, r.stderr[-2000:]


def test_worker_thread_deep_stack(tmp_path):
    # 50,000 nested lifted calls on a worker: far past pthread's default 512 KB host stack.
    asm = r"""
    push 0
    push 0
    push 0
    push {worker}
    push 0
    push 0
    call dword ptr [{{IAT_CreateThread}}]
    push -1
    push eax
    call dword ptr [{{IAT_WaitForSingleObject}}]
    push dword ptr [depth]
    call dword ptr [{{IAT_ExitProcess}}]
depth: .long 0
.org 0x400
    mov ecx, 50000
    call rec
    ret 4
.org 0x500
rec:
    push ebp
    mov ebp, esp
    sub esp, 32
    inc dword ptr [depth]
    dec ecx
    jz 1f
    call rec
1:  mov esp, ebp
    pop ebp
    ret
""".format(worker=WORKER)
    b = _engine(tmp_path, asm, {K32: THR}, functions=(BASE, WORKER, BASE + 0x500))
    r = _run(tmp_path, b, timeout=30)
    assert r.returncode == 50000 & 0xFF, r.stderr[-2000:]


def _tls_image(asm):
    # .text at 0x401000; the TLS directory sits at tlsdir (offset 0x700), its template at 0x780.
    imports = {K32: THR}
    pad = [(".text", b"\xcc" * 0x800, CODE)]
    slots = {f"IAT_{fn}": iat_va(pad, imports, K32, fn) for fn in THR}
    code = asm32(asm.format(**slots), base=BASE)
    return make_pe([(".text", code.ljust(0x800, b"\xcc"), CODE)], imports, 0x1000, data_dirs={9: (0x1700, 24)})


def test_tls_per_thread(tmp_path):
    asm = r"""
    push 0
    push 0
    push 0
    push 0x401400
    push 0
    push 0
    call dword ptr [{IAT_CreateThread}]
    push -1
    push eax
    call dword ptr [{IAT_WaitForSingleObject}]
    mov eax, dword ptr fs:[0x2c]
    mov ecx, dword ptr [tls_index]
    mov eax, dword ptr [eax + ecx*4]
    cmp dword ptr [eax], 0x1234
    jne bad_main
    cmp dword ptr [wseen], 0x1234
    jne bad_worker
    push 0
    call dword ptr [{IAT_ExitProcess}]
bad_main:
    push 1
    call dword ptr [{IAT_ExitProcess}]
bad_worker:
    push 2
    call dword ptr [{IAT_ExitProcess}]
wseen: .long 0
tls_index: .long 0xFFFF
.org 0x400
    mov eax, dword ptr fs:[0x2c]
    mov ecx, dword ptr [tls_index]
    mov eax, dword ptr [eax + ecx*4]
    mov ecx, dword ptr [eax]
    mov dword ptr [wseen], ecx
    mov dword ptr [eax], 5
    ret 4
.org 0x700
tlsdir:
    .long tmpl
    .long tmpl + 4
    .long tls_index
    .long 0
    .long 12
    .long 0
.org 0x780
tmpl: .long 0x1234
"""
    pe = _tls_image(asm)
    b = guest_engine(tmp_path, pe, [BASE, BASE + 0x400])
    r = _run(tmp_path, b, timeout=20)
    assert r.returncode == 0, r.stderr[-2000:]


def test_exit_from_worker_thread(tmp_path):
    asm = r"""
    push offset hk
    push 0xF003F
    push 0
    push offset key
    push 0x80000001
    call dword ptr [{{IAT_RegOpenKeyExA}}]
    push offset disp
    push offset hk
    push 0
    push 0xF003F
    push 0
    push 0
    push 0
    push offset key
    push 0x80000001
    call dword ptr [{{IAT_RegCreateKeyExA}}]
    push 4
    push offset val
    push 4
    push 0
    push offset vname
    push dword ptr [hk]
    call dword ptr [{{IAT_RegSetValueExA}}]
    push 0
    push 0
    push 0
    push {worker}
    push 0
    push 0
    call dword ptr [{{IAT_CreateThread}}]
    push -1
    push eax
    call dword ptr [{{IAT_WaitForSingleObject}}]
    push 1
    call dword ptr [{{IAT_ExitProcess}}]
key: .asciz "Software\\BW2Exit"
vname: .asciz "flushed"
hk: .long 0
disp: .long 0
val: .long 7
.org 0x400
    push 9
    call dword ptr [{{IAT_ExitProcess}}]
    ret 4
""".format(worker=WORKER)
    b = _engine(tmp_path, asm, {K32: THR, "ADVAPI32.dll": ["RegOpenKeyExA", "RegCreateKeyExA", "RegSetValueExA"]},
                functions=(BASE, WORKER))
    r = _run(tmp_path, b, timeout=20)
    assert r.returncode == 9, r.stderr[-2000:]
    assert "flushed" in (tmp_path / "support" / "registry.txt").read_text()
