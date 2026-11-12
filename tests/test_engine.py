"""The engine: lifted guests linked with the win32hle host and the BW2 runtime."""
import os

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


# ---- Task 4: CPUID and RDTSC -------------------------------------------------

def _checks(pairs):
    """asm: cmp reg, value / jne fail_<n> for each (reg, value); exit 0, or n at the first miss."""
    body = "".join(f"    cmp {reg}, {val:#x}\n    jne fail_{i + 1}\n" for i, (reg, val) in enumerate(pairs))
    fails = "".join(f"fail_{i + 1}:\n    push {i + 1}\n    call dword ptr [{{IAT_ExitProcess}}]\n"
                    for i in range(len(pairs)))
    return body, fails


def test_cpuid_answers(tmp_path):
    l0, f0 = _checks([("eax", 1), ("ebx", 0x756E6547), ("edx", 0x49656E69), ("ecx", 0x6C65746E)])
    l1, f1 = _checks([("eax", 0x6F6), ("ebx", 0x00010800), ("ecx", 0), ("edx", 0x0789FBFF)])
    lb, fb = _checks([("eax", 0x20325742)])          # "BW2 "
    lx, fx = _checks([("eax", 0x80000004), ("edx", 0)])
    le, fe = _checks([("edx", 0)])
    # renumber each block's labels so they do not collide
    def tag(code, t):
        return code.replace("fail_", f"fail_{t}_")
    asm = ("    xor eax, eax\n    xor ecx, ecx\n    cpuid\n" + tag(l0, "a") +
           "    mov eax, 1\n    xor ecx, ecx\n    cpuid\n" + tag(l1, "b") +
           "    mov eax, 0x80000002\n    cpuid\n" + tag(lb, "c") +
           "    mov eax, 0x80000000\n    cpuid\n" + tag(lx, "d") +
           "    mov eax, 0x80000001\n    cpuid\n" + tag(le, "e") +
           "    push 0\n    call dword ptr [{IAT_ExitProcess}]\n" +
           tag(f0, "a").replace("push ", "push 10+") + tag(f1, "b").replace("push ", "push 20+") +
           tag(fb, "c").replace("push ", "push 30+") + tag(fx, "d").replace("push ", "push 40+") +
           tag(fe, "e").replace("push ", "push 50+"))
    b = _engine(tmp_path, asm, {K32: ["ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 0, f"check {r.returncode} failed\n" + r.stderr[-2000:]


def test_rdtsc_tracks_time(tmp_path):
    asm = r"""
    rdtsc
    mov dword ptr [lo], eax
    mov dword ptr [hi], edx
    push 200
    call dword ptr [{IAT_Sleep}]
    rdtsc
    sub eax, dword ptr [lo]
    sbb edx, dword ptr [hi]
    cmp edx, 0
    jne too_big
    cmp eax, 450000000
    jb too_small
    cmp eax, 1200000000
    ja too_big
    push 0
    call dword ptr [{IAT_ExitProcess}]
too_small:
    push 1
    call dword ptr [{IAT_ExitProcess}]
too_big:
    push 2
    call dword ptr [{IAT_ExitProcess}]
lo: .long 0
hi: .long 0
"""
    b = _engine(tmp_path, asm, {K32: ["Sleep", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 0, f"code {r.returncode} (1: below 3 GHz, 2: above)\n" + r.stderr[-2000:]


# ---- Task 5: lifted DLLs -----------------------------------------------------

DLL_BASE = 0x10000000
DLL_TEXT = DLL_BASE + 0x1000
LIB_ASM = r"""
dllmain:                       # DllMain(hmod, reason, reserved): count attaches
    inc dword ptr [attaches]
    mov eax, 1
    ret 12
.org 0x100
add2:
    mov eax, dword ptr [esp + 4]
    add eax, dword ptr [esp + 8]
    ret 8
.org 0x200
get_count:
    mov eax, dword ptr [attaches]
    ret
.org 0x300
attaches: .long 0
"""


def _lib(image_base=DLL_BASE):
    code = asm32(LIB_ASM, base=image_base + 0x1000)
    return make_pe([(".text", code.ljust(0x400, b"\xcc"), CODE)], {}, 0x1000, image_base=image_base,
                   exports={"Add2": 0x1100, "GetCount": 0x1200}, dll_name="Lib.dll")


MAIN_LOADS_LIB = r"""
    push offset libname
    call dword ptr [{IAT_LoadLibraryA}]
    push offset libname
    call dword ptr [{IAT_LoadLibraryA}]
    mov esi, eax
    push offset n_add2
    push esi
    call dword ptr [{IAT_GetProcAddress}]
    push 4
    push 3
    call eax
    mov edi, eax
    push offset n_count
    push esi
    call dword ptr [{IAT_GetProcAddress}]
    call eax
    imul edi, edi, 10
    add edi, eax
    push edi
    call dword ptr [{IAT_ExitProcess}]
libname: .asciz "plugins\\Lib.dll"
n_add2: .asciz "Add2"
n_count: .asciz "GetCount"
"""
LIB_IMPORTS = {K32: ["LoadLibraryA", "GetProcAddress", "ExitProcess"]}


def test_lifted_dll_loads_at_base(tmp_path):
    pe = _image(MAIN_LOADS_LIB, LIB_IMPORTS)
    b = guest_engine(tmp_path, pe, [BASE],
                     dlls={"plugins/Lib.dll": (_lib(), [DLL_TEXT, DLL_TEXT + 0x100, DLL_TEXT + 0x200])})
    r = _run(tmp_path, b, "--lifted-dll", "lib.dll=0x10000000")
    assert r.returncode == 71, r.stderr[-2000:]           # Add2(3, 4) = 7, DllMain ran once


def test_lifted_dll_base_taken_fails_loudly(tmp_path):
    pe = _image(MAIN_LOADS_LIB, LIB_IMPORTS)
    b = guest_engine(tmp_path, pe, [BASE],
                     dlls={"plugins/Lib.dll": (_lib(0x400000), [])})
    r = _run(tmp_path, b, "--lifted-dll", "lib.dll=0x400000")
    assert r.returncode not in (0, 71) and "lib.dll" in r.stderr.lower() and "0x00400000" in r.stderr, r.stderr[-2000:]


# ---- Task 6: EH landing pads -------------------------------------------------

def test_no_stale_eh_frames_after_return(tmp_path):
    from test_lift import EH_FUNC_ASM, EH_FUNCS
    asm = EH_FUNC_ASM.replace("func:", r"""
start:
    mov esi, 1000
again:
    call func
    dec esi
    jnz again
    push 0
    call dword ptr [{IAT_ExitProcess}]
.org 0x40
func:""", 1).replace(".org 0x1", ".org 0x40+0x1").replace(".org 0x2", ".org 0x40+0x2").replace(".org 0x3", ".org 0x40+0x3")
    funcs = [BASE] + [f + 0x40 for f in EH_FUNCS]
    pe = _image(asm, {K32: ["ExitProcess"]})
    b = guest_engine(tmp_path, pe, funcs)
    r = _run(tmp_path, b, "--selftest-eh-depth")
    assert r.returncode == 0 and "eh depth 0" in r.stderr, r.stderr[-2000:]


# ---- Task 7: guest C++ exception dispatch --------------------------------------
# MSVC 7.1 EH data as eh32 reads it:
#   FuncInfo          magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap
#   UnwindMapEntry    toState, action
#   TryBlockMapEntry  tryLow, tryHigh, catchHigh, nCatches, pHandlerArray
#   HandlerType       adjectives, pType, dispCatchObj, addressOfHandler
#   ThrowInfo         attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray
#   CatchableType     properties, pType, mdisp, pdisp, vdisp, size, copyFunction
#   TypeDescriptor    pVFTable, spare, name

EH_COMMON = r"""
cxx_throw:                       # _CxxThrowException(obj, info), stdcall, as MSVC's
    push ebp
    mov ebp, esp
    sub esp, 12
    mov dword ptr [ebp - 12], 0x19930520
    mov eax, dword ptr [ebp + 8]
    mov dword ptr [ebp - 8], eax
    mov eax, dword ptr [ebp + 12]
    mov dword ptr [ebp - 4], eax
    lea eax, [ebp - 12]
    push eax
    push 3
    push 1
    push 0xE06D7363
    call dword ptr [{IAT_RaiseException}]
    int3
"""
EH_DATA = r"""
td_int: .long 0, 0
    .asciz ".H"
.p2align 2
td_float: .long 0, 0
    .asciz ".M"
.p2align 2
ct_int: .long 1, td_int, 0, -1, 0, 4, 0
cta_int: .long 1, ct_int
info_int: .long 0, 0, 0, cta_int
info_int_dtor: .long 0, obj_dtor, 0, cta_int
obj_int: .long 5
dtor_count: .long 0
"""


SLOT = 0x100


class EH:
    """An image built from pieces, each at its own SLOT-byte slot; functions are catalogued."""

    def __init__(self):
        self.pieces, self.funcs = [], []

    def func(self, body):
        self.funcs.append(BASE + SLOT * len(self.pieces))
        self.pieces.append(body)

    def data(self, body):
        self.pieces.append(body)

    def catcher(self, name, try_body, handlers, cont, epilogue_ret="ret"):
        """A function with an MSVC EH frame: locals [ebp-0x14] (catch variable) and
        [ebp-0x18] (= 100, set before the try); one try block, state 0, whose
        handlers are (type label or 0, catch funclet body)."""
        self.func(rf"""
{name}:
    push ebp
    mov ebp, esp
    push -1
    push offset {name}_handler
    mov eax, dword ptr fs:[0]
    push eax
    mov dword ptr fs:[0], esp
    sub esp, 12
    mov dword ptr [ebp - 0x10], esp
    mov dword ptr [ebp - 0x14], 0
    mov dword ptr [ebp - 0x18], 100
    mov dword ptr [ebp - 4], 0
{try_body}
    mov dword ptr [ebp - 4], -1
    mov eax, 1
    jmp {name}_done
{name}_cont:
{cont}
{name}_done:
    mov ecx, dword ptr [ebp - 0x0C]
    mov dword ptr fs:[0], ecx
    mov esp, ebp
    pop ebp
    {epilogue_ret}
""")
        for i, (_t, body) in enumerate(handlers):
            self.func(f"{name}_catch{i}:\n{body}\n")
        self.func(f"{name}_handler:\n    mov eax, offset {name}_fi\n    .byte 0xE9\n"
                  f"    .long cxx_frame_handler - . - 4\n")
        hs = "\n".join(f"    .long 0, {t}, 0xFFFFFFEC, {name}_catch{i}" for i, (t, _b) in enumerate(handlers))
        self.data(f"{name}_fi: .long 0x19930520, 1, {name}_unwind, 1, {name}_try\n"
                  f"{name}_unwind: .long -1, 0\n"
                  f"{name}_try: .long 0, 0, 1, {len(handlers)}, {name}_hs\n{name}_hs:\n{hs}\n")

    def build(self, tmp_path, start):
        e = EH()
        e.func(start)
        e.func(EH_COMMON)
        e.func("cxx_frame_handler:\n    int3\n")
        e.func("obj_dtor:\n    cmp ecx, offset obj_int\n    jne 1f\n    inc dword ptr [dtor_count]\n1:  ret\n")
        e.pieces += self.pieces
        e.funcs += [f + SLOT * 4 for f in self.funcs]
        e.data(EH_DATA)
        asm = "".join(f".org {SLOT * i:#x}\n{p}\n" for i, p in enumerate(e.pieces))
        pe = _image(asm, {K32: ["RaiseException", "ExitProcess"]}, text_len=SLOT * len(e.pieces) + 0x200)
        return guest_engine(tmp_path, pe, e.funcs)


EXIT_EAX = "    push eax\n    call dword ptr [{IAT_ExitProcess}]\n"
THROW_INT = "    push offset info_int\n    push offset obj_int\n    call cxx_throw\n"
CATCH_RET_CONT = "    mov eax, offset {name}_cont\n    ret"
SUM_CONT = "    mov eax, dword ptr [ebp - 0x14]\n    add eax, dword ptr [ebp - 0x18]"


def _catcher(eh, name, try_body, types=("td_int",), cont=SUM_CONT):
    eh.catcher(name, try_body, [(t, CATCH_RET_CONT.format(name=name)) for t in types], cont)


def test_throw_caught_one_frame_up(tmp_path):
    eh = EH()
    _catcher(eh, "c1", "    call thrower")
    eh.func("thrower:\n" + THROW_INT + "    ret\n")
    b = eh.build(tmp_path, "    call c1\n" + EXIT_EAX)
    r = _run(tmp_path, b, "--trace-eh")
    assert r.returncode == 105, r.stderr[-3000:]          # 5 caught + the local 100


def test_unwind_runs_destructors(tmp_path):
    eh = EH()
    _catcher(eh, "c1", "    call mid")
    eh.func(r"""
mid:                              # an EH frame with only an unwind action for state 0
    push ebp
    mov ebp, esp
    push -1
    push offset mid_handler
    mov eax, dword ptr fs:[0]
    push eax
    mov dword ptr fs:[0], esp
    mov dword ptr [ebp - 4], 0
    call thrower
    mov ecx, dword ptr [ebp - 0x0C]
    mov dword ptr fs:[0], ecx
    mov esp, ebp
    pop ebp
    ret
""")
    eh.func("mid_dtor:\n    inc dword ptr [dtor_count]\n    ret\n")
    eh.func("mid_handler:\n    mov eax, offset mid_fi\n    .byte 0xE9\n    .long cxx_frame_handler - . - 4\n")
    eh.data("mid_fi: .long 0x19930520, 1, mid_unwind, 0, 0\nmid_unwind: .long -1, mid_dtor\n")
    eh.func("thrower:\n" + THROW_INT + "    ret\n")
    b = eh.build(tmp_path, "    call c1\n    add eax, dword ptr [dtor_count]\n" + EXIT_EAX)
    r = _run(tmp_path, b)
    assert r.returncode == 106, r.stderr[-3000:]          # 105 + one destructor


def test_catch_all(tmp_path):
    eh = EH()
    _catcher(eh, "c1", "    call thrower", types=("0",), cont="    mov eax, 77")
    eh.func("thrower:\n" + THROW_INT + "    ret\n")
    b = eh.build(tmp_path, "    call c1\n" + EXIT_EAX)
    r = _run(tmp_path, b)
    assert r.returncode == 77, r.stderr[-3000:]


def test_type_mismatch_goes_outward(tmp_path):
    eh = EH()
    _catcher(eh, "outer", "    call inner")
    _catcher(eh, "inner", "    call thrower", types=("td_float",), cont="    mov eax, 9")
    eh.func("thrower:\n" + THROW_INT + "    ret\n")
    b = eh.build(tmp_path, "    call outer\n" + EXIT_EAX)
    r = _run(tmp_path, b)
    assert r.returncode == 105, r.stderr[-3000:]


def test_rethrow_caught_outside(tmp_path):
    eh = EH()
    _catcher(eh, "outer", "    call inner")
    eh.catcher("inner", "    call thrower",
               [("td_int", "    push 0\n    push 0\n    call cxx_throw\n    int3")], "    mov eax, 9")
    eh.func("thrower:\n" + THROW_INT + "    ret\n")
    b = eh.build(tmp_path, "    call outer\n" + EXIT_EAX)
    r = _run(tmp_path, b)
    assert r.returncode == 105, r.stderr[-3000:]


def test_recursive_catch_resumes_right_activation(tmp_path):
    eh = EH()
    # rec(n): try { if (n == 3) throw n; return rec(n + 1); } catch (int e) { return e * 10 + n; }
    eh.catcher("rec", r"""
    mov eax, dword ptr [ebp + 8]
    cmp eax, 3
    jne rec_down
    mov dword ptr [obj_int], eax
""" + THROW_INT + r"""
rec_down:
    inc eax
    push eax
    call rec
    add esp, 4
    mov dword ptr [ebp - 4], -1
    jmp rec_done""", [("td_int", CATCH_RET_CONT.format(name="rec"))],
               "    mov eax, dword ptr [ebp - 0x14]\n    imul eax, eax, 10\n    add eax, dword ptr [ebp + 8]")
    b = eh.build(tmp_path, "    push 1\n    call rec\n    add esp, 4\n" + EXIT_EAX)
    r = _run(tmp_path, b)
    assert r.returncode == 33, r.stderr[-3000:]           # depth 3 caught its own throw


def test_exception_object_destroyed_after_catch(tmp_path):
    eh = EH()
    # the catch funclet records the count while the catch runs; the continuation adds it after
    during = ("    mov eax, dword ptr [dtor_count]\n    mov dword ptr [ebp - 0x14], eax\n"
              "    mov eax, offset c1_cont\n    ret")
    eh.catcher("c1", "    call thrower", [("td_int", during)],
               "    mov eax, dword ptr [ebp - 0x14]\n    imul eax, eax, 10\n    add eax, dword ptr [dtor_count]")
    eh.func("thrower:\n    push offset info_int_dtor\n    push offset obj_int\n    call cxx_throw\n    ret\n")
    b = eh.build(tmp_path, "    call c1\n" + EXIT_EAX)
    r = _run(tmp_path, b)
    assert r.returncode == 1, r.stderr[-3000:]            # 0 during the catch, 1 after it


def test_uncaught_throw_named(tmp_path):
    eh = EH()
    eh.func("thrower:\n" + THROW_INT + "    ret\n")
    b = eh.build(tmp_path, "    call thrower\n    push 0\n    call dword ptr [{IAT_ExitProcess}]\n")
    r = _run(tmp_path, b)
    assert r.returncode != 0 and "uncaught C++ exception" in r.stderr and ".H" in r.stderr, r.stderr[-3000:]


def test_other_raise_is_loud(tmp_path):
    eh = EH()
    b = eh.build(tmp_path, "    push 0\n    push 0\n    push 0\n    push 0x12345678\n"
                           "    call dword ptr [{IAT_RaiseException}]\n    push 0\n    call dword ptr [{IAT_ExitProcess}]\n")
    r = _run(tmp_path, b)
    assert r.returncode != 0 and "0x12345678" in r.stderr, r.stderr[-3000:]


# ---- Task 8: heap quarantine, handle table -----------------------------------

HEAP = ["GetProcessHeap", "HeapAlloc", "HeapFree", "CreateEventA", "CloseHandle", "ExitProcess"]


def test_freed_block_not_reused_at_once(tmp_path):
    asm = r"""
    call dword ptr [{IAT_GetProcessHeap}]
    mov ebx, eax
    push 64
    push 0
    push ebx
    call dword ptr [{IAT_HeapAlloc}]
    mov esi, eax
    mov edi, eax
    mov ecx, 16
    mov eax, 0xABABABAB
    rep stosd
    push esi
    push 0
    push ebx
    call dword ptr [{IAT_HeapFree}]
    mov edi, 100
again:
    push 64
    push 0
    push ebx
    call dword ptr [{IAT_HeapAlloc}]
    cmp eax, esi
    je reused
    dec edi
    jnz again
    cmp dword ptr [esi], 0xABABABAB
    jne clobbered
    cmp dword ptr [esi + 60], 0xABABABAB
    jne clobbered
    push 0
    call dword ptr [{IAT_ExitProcess}]
reused:
    push 1
    call dword ptr [{IAT_ExitProcess}]
clobbered:
    push 2
    call dword ptr [{IAT_ExitProcess}]
"""
    b = _engine(tmp_path, asm, {K32: HEAP})
    r = _run(tmp_path, b)
    assert r.returncode == 0, f"code {r.returncode} (1: reused at once, 2: contents changed)\n" + r.stderr[-2000:]


def test_many_handles(tmp_path):
    asm = r"""
    call dword ptr [{IAT_GetProcessHeap}]
    push 40000
    push 0
    push eax
    call dword ptr [{IAT_HeapAlloc}]
    mov esi, eax
    xor edi, edi
open:
    push 0
    push 0
    push 0
    push 0
    call dword ptr [{IAT_CreateEventA}]
    test eax, eax
    jz full
    mov dword ptr [esi + edi*4], eax
    inc edi
    cmp edi, 10000
    jne open
close:
    dec edi
    push dword ptr [esi + edi*4]
    call dword ptr [{IAT_CloseHandle}]
    test edi, edi
    jnz close
    push 0
    call dword ptr [{IAT_ExitProcess}]
full:
    push 1
    call dword ptr [{IAT_ExitProcess}]
"""
    b = _engine(tmp_path, asm, {K32: HEAP})
    r = _run(tmp_path, b)
    assert r.returncode == 0, r.stderr[-2000:]


# ---- Task 9: disc and driver tripwire ----------------------------------------

def test_deviceiocontrol_trips(tmp_path):
    asm = r"""
    push 0
    push 0
    push 0
    push 0
    push 0
    push 0
    push 0x2D1400
    push 0x100
    call dword ptr [{IAT_DeviceIoControl}]
    push 0
    call dword ptr [{IAT_ExitProcess}]
"""
    b = _engine(tmp_path, asm, {K32: ["DeviceIoControl", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 3 and "TRIPWIRE: DeviceIoControl" in r.stderr, r.stderr[-2000:]
    assert "VM: no" in r.stderr


def test_drive_type_from_game_trips_too(tmp_path):
    # The VM could ask through an ordinary game function, so every drive question stops,
    # whoever asks; the user decides (a call site they approve goes on an allow list).
    asm = r"""
    push offset root
    call dword ptr [{IAT_GetDriveTypeA}]
    cmp eax, 3
    jne bad
    push 0
    call dword ptr [{IAT_ExitProcess}]
bad:
    push 1
    call dword ptr [{IAT_ExitProcess}]
root: .asciz "C:\\"
"""
    b = _engine(tmp_path, asm, {K32: ["GetDriveTypeA", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 3 and "TRIPWIRE: GetDriveTypeA" in r.stderr and "VM: no" in r.stderr, r.stderr[-2000:]


def test_drive_type_from_vm_range_trips(tmp_path):
    imports = {K32: ["GetDriveTypeA", "ExitProcess"]}
    pad = [(".text", b"\xcc" * 0x100, CODE), ("stxt774", b"\xcc" * 0x100, CODE)]
    slot = lambda fn: iat_va(pad, imports, K32, fn)
    main = asm32(f"""
    mov eax, 0x402000
    call eax
    push 0
    call dword ptr [{slot("ExitProcess"):#x}]
""", base=BASE)
    vm = asm32(f"""
    push offset root
    call dword ptr [{slot("GetDriveTypeA"):#x}]
    ret
root: .asciz "C:\\\\"
""", base=0x402000)
    pe = make_pe([(".text", main.ljust(0x100, b"\xcc"), CODE), ("stxt774", vm.ljust(0x100, b"\xcc"), CODE)],
                 imports, 0x1000)
    b = guest_engine(tmp_path, pe, [BASE, 0x402000], code_sections=(".text", "stxt774"))
    r = _run(tmp_path, b)
    assert r.returncode == 3 and "TRIPWIRE: GetDriveTypeA" in r.stderr and "VM: yes" in r.stderr, r.stderr[-2000:]


# ---- Task 10: BW2's own shims ---------------------------------------------------

def test_shims_selftest(tmp_path):
    import subprocess
    from bw2.build import BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs, engine_sources, sdk_path
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "shims_selftest.c"]
    exe = tmp_path / "shims_selftest"
    subprocess.run([str(check_tools()), "-O1", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()), "-I", str(RECOMP32),
                    "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs), *engine_libs(), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, cwd=tmp_path)
    assert r.returncode == 0 and "shims selftest: 0 failed" in r.stdout, r.stdout + r.stderr[-2000:]


def test_ret_to_pushed_address_jumps(tmp_path):
    # f: push offset g; ret   -- as the VM's exit does: on a CPU this runs g, whose ret goes home
    asm = r"""
    call f
    push eax
    call dword ptr [{IAT_ExitProcess}]
.org 0x40
f:
    push offset g
    ret
.org 0x80
g:
    mov eax, 42
    ret
"""
    pe = _image(asm, {K32: ["ExitProcess"]})
    b = guest_engine(tmp_path, pe, [BASE, BASE + 0x40, BASE + 0x80], computed_ret=True)
    r = _run(tmp_path, b)
    assert r.returncode == 42, r.stderr[-2000:]


def test_rewritten_push_operand_is_used(tmp_path):
    asm = r"""
    mov dword ptr [stub + 1], 77
    call stub
    push eax
    call dword ptr [{IAT_ExitProcess}]
.org 0x40
stub:
    .byte 0x68
    .long 0
    pop eax
    ret
"""
    pe = _image(asm, {K32: ["ExitProcess"]})
    b = guest_engine(tmp_path, pe, [BASE, BASE + 0x40], runtime_push={BASE + 0x40})
    r = _run(tmp_path, b)
    assert r.returncode == 77, r.stderr[-2000:]


# ---- Final review fixes --------------------------------------------------------

def test_shcreatedirectory_in_install_folder_stays_out_of_game_tree(tmp_path):
    asm = r"""
    push 0
    push offset path
    push 0
    call dword ptr [{IAT_SHCreateDirectoryExA}]
    push eax
    call dword ptr [{IAT_ExitProcess}]
path: .asciz "C:\\Program Files\\EA GAMES\\Black & White 2\\NewDir\\Sub"
"""
    b = _engine(tmp_path, asm, {K32: ["ExitProcess"], "SHELL32.dll": ["SHCreateDirectoryExA"]})
    before = sorted(p.relative_to(tmp_path / "game").as_posix() for p in (tmp_path / "game").rglob("*"))
    r = _run(tmp_path, b)
    assert r.returncode == 0, r.stderr[-2000:]
    assert sorted(p.relative_to(tmp_path / "game").as_posix() for p in (tmp_path / "game").rglob("*")) == before
    assert (tmp_path / "support" / "Game" / "NewDir" / "Sub").is_dir()


def _vm_caller(tmp_path, vm_asm, imports):
    # main calls a function in a section named stxt774 (the protection VM's)
    pad = [(".text", b"\xcc" * 0x100, CODE), ("stxt774", b"\xcc" * 0x100, CODE)]
    slots = {f"IAT_{fn}": iat_va(pad, imports, dll, fn) for dll, fns in imports.items() for fn in fns}
    main = asm32("    mov eax, 0x402000\n    call eax\n    push 0\n    call dword ptr [{IAT_ExitProcess}]\n"
                 .format(**slots), base=BASE)
    vm = asm32(vm_asm.format(**slots), base=0x402000)
    pe = make_pe([(".text", main.ljust(0x100, b"\xcc"), CODE), ("stxt774", vm.ljust(0x100, b"\xcc"), CODE)],
                 imports, 0x1000)
    return guest_engine(tmp_path, pe, [BASE, 0x402000], code_sections=(".text", "stxt774"))


def test_vm_import_other_than_thread_id_trips(tmp_path):
    b = _vm_caller(tmp_path, "    call dword ptr [{IAT_GetTickCount}]\n    ret\n", {K32: ["GetTickCount", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 3 and "TRIPWIRE: GetTickCount" in r.stderr and "VM: yes" in r.stderr, r.stderr[-2000:]


def test_vm_thread_id_is_allowed(tmp_path):
    b = _vm_caller(tmp_path, "    call dword ptr [{IAT_GetCurrentThreadId}]\n    ret\n",
                   {K32: ["GetCurrentThreadId", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 0, r.stderr[-2000:]


def test_nt_device_path_trips(tmp_path):
    asm = r"""
    push 0
    push 0x80
    push 3
    push 0
    push 1
    push 0x80000000
    push offset name
    call dword ptr [{IAT_CreateFileA}]
    push 0
    call dword ptr [{IAT_ExitProcess}]
name: .asciz "\\??\\CdRom0"
"""
    b = _engine(tmp_path, asm, {K32: ["CreateFileA", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 3 and "TRIPWIRE: CreateFileA" in r.stderr, r.stderr[-2000:]


def _write_attempt(name):
    return r"""
    push 0
    push 0x80
    push 2
    push 0
    push 0
    push 0x40000000
    push offset name
    call dword ptr [{IAT_CreateFileA}]
    cmp eax, -1
    jne opened
    push 0
    call dword ptr [{IAT_ExitProcess}]
opened:
    push 1
    call dword ptr [{IAT_ExitProcess}]
name: .asciz "%s"
""" % name


def test_no_drive_z(tmp_path):
    # pcrecomp's Z: is the Mac's whole disk; the game must not reach it
    target = tmp_path / "outside.txt"
    guest = "Z:" + str(target).replace("/", "\\\\")
    b = _engine(tmp_path, _write_attempt(guest), {K32: ["CreateFileA", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 0 and not target.exists(), r.stderr[-2000:]


def test_relative_path_cannot_climb_out(tmp_path):
    # from the install folder (the host cwd is the game tree), ..\..\ must not leave the allowed roots
    b = _engine(tmp_path, _write_attempt(r"..\\..\\escaped.txt"), {K32: ["CreateFileA", "ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 0, r.stderr[-2000:]
    assert not list(tmp_path.parent.glob("escaped.txt")) and not (tmp_path / "escaped.txt").exists()


# ---- Plan 4 Task 1: the main thread serves the window ---------------------------

def test_main_sync_runs_on_main_thread(tmp_path, monkeypatch):
    monkeypatch.setenv("BW2_HEADLESS", "1")
    b = _engine(tmp_path, "ret", {K32: ["ExitProcess"]})
    r = _run(tmp_path, b, "--selftest-mainsync")
    assert r.returncode == 0 and "main sync ok" in r.stdout, r.stdout + r.stderr[-2000:]


# ---- Plan 4 Task 2: x87 precision control --------------------------------------

def test_x87_single_precision_mode(tmp_path):
    import struct
    single = struct.unpack("<Q", struct.pack("<d", struct.unpack("<f", struct.pack("<f", 1 / 3))[0]))[0]
    double = struct.unpack("<Q", struct.pack("<d", 1 / 3))[0]
    # exit code: bit 0 = PC=24 result is 1/3 rounded to float, bit 1 = PC=53 result is the double 1/3
    asm = r"""
    xor ebx, ebx
    fldcw word ptr [cw24]
    fld1
    fld dword ptr [three]
    fdivp st(1), st
    fstp qword ptr [out]
    mov eax, dword ptr [out]
    cmp eax, dword ptr [want24]
    jne 1f
    mov eax, dword ptr [out + 4]
    cmp eax, dword ptr [want24 + 4]
    jne 1f
    or ebx, 1
1:  fldcw word ptr [cw53]
    fld1
    fld dword ptr [three]
    fdivp st(1), st
    fstp qword ptr [out]
    mov eax, dword ptr [out]
    cmp eax, dword ptr [want53]
    jne 2f
    mov eax, dword ptr [out + 4]
    cmp eax, dword ptr [want53 + 4]
    jne 2f
    or ebx, 2
2:  push ebx
    call dword ptr [{IAT_ExitProcess}]
.org 0x200
cw24: .short 0x007F
cw53: .short 0x027F
three: .float 3.0
.p2align 3
out: .quad 0
want24: .quad WANT24
want53: .quad WANT53
""".replace("WANT24", hex(single)).replace("WANT53", hex(double))
    b = _engine(tmp_path, asm, {K32: ["ExitProcess"]})
    r = _run(tmp_path, b)
    assert r.returncode == 3, f"exit {r.returncode}: bit 0 = single-precision result right, bit 1 = double\n{r.stderr[-2000:]}"


# ---- Plan 4 Task 10: a real window (opens one on screen; opt in with BW2_GUI=1) ----

@pytest.mark.skipif(os.environ.get("BW2_GUI") != "1", reason="opens a window: set BW2_GUI=1")
def test_window_device_presents(tmp_path):
    b = _engine(tmp_path, "ret", {K32: ["ExitProcess"]})
    r = _run(tmp_path, b, "--selftest-window")
    assert r.returncode == 0 and "window ok 60" in r.stdout, r.stdout + r.stderr[-2000:]


# ---- Plan 6 Task 4: the game's window takes Mac input (opens one on screen; opt in with BW2_GUI=1) ----

@pytest.mark.skipif(os.environ.get("BW2_GUI") != "1", reason="opens a window: set BW2_GUI=1")
def test_window_input(tmp_path):
    # synthetic NSEvents posted to a windowed 640x480 device's window reach the game window's procedure
    # as Windows messages: a click at the view's centre, the A key, a 3-line scroll, then the close button
    b = _engine(tmp_path, "ret", {K32: ["ExitProcess"]})
    r = _run(tmp_path, b, "--selftest-input-window")
    assert r.returncode == 0, r.stdout + r.stderr[-2000:]
    lines = r.stdout.splitlines()
    want = ["WM_LBUTTONDOWN 320,240", "WM_LBUTTONUP 320,240", "WM_KEYDOWN 41 001E0001", "WM_MOUSEWHEEL 360",
            "WM_CLOSE", "input window ok"]
    it = iter(lines)
    missing = [w for w in want if not any(ln == w for ln in it)]
    assert not missing, f"missing (in order) {missing}:\n" + r.stdout + r.stderr[-2000:]


# ---- Plan 6 Task 5: the cursor on a real window (opens one and moves the pointer; opt in with BW2_GUI=1) ----

@pytest.mark.skipif(os.environ.get("BW2_GUI") != "1", reason="opens a window: set BW2_GUI=1")
def test_window_cursor(tmp_path):
    # the pointer parked outside the window before it opens; the game's DirectInput setup and mode switch
    # with no cursor call after them: the pointer is warped into the picture (to its centre, 320,240),
    # detached and hidden. Hidden away, it is shown and attached; back (the game's loop acquires again),
    # parked again. The exact point is printed, not asserted: a hand on the mouse moves the pointer in the
    # moment between the warp and the detach. macOS may refuse to bring a terminal-started app back to the
    # front; then the window is not key and the pointer must stay shown and attached.
    b = _engine(tmp_path, "ret", {K32: ["ExitProcess"]})
    r = _run(tmp_path, b, "--selftest-cursor-window")
    out = r.stdout + r.stderr[-2000:]
    assert r.returncode == 0 and "cursor window ok" in r.stdout, out
    assert "dinput setup 0x0" in r.stdout, out
    state = {ln.split(":")[0][len("cursor "):]: ln.split(": ", 1)[1] for ln in r.stdout.splitlines()
             if ln.startswith("cursor ") and ": key" in ln}
    assert state["exclusive"].startswith("key 1 hidden 1 detached 1 inside 1 at "), out
    assert state["hidden"].startswith("key 0 hidden 0 detached 0 "), out
    if state["back"].startswith("key 1"):
        assert state["back"].startswith("key 1 hidden 1 detached 1 inside 1 at "), out
    else:
        assert state["back"].startswith("key 0 hidden 0 detached 0 "), out
