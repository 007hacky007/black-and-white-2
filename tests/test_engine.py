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
