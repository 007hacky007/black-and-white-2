import pefile
import pytest

from bw2.build import ToolsMissing, check_tools
from fixtures import CODE, asm32, make_pe

try:
    check_tools()
except ToolsMissing:
    pytest.skip("needs Xcode clang to assemble test code", allow_module_level=True)

from bw2.pe32emu import Pe32Host, Win32Unimplemented  # noqa: E402

ENTRY = 0x401000


def build(asm: str, imports: dict[str, list[str]] | None = None) -> bytes:
    """A PE whose .text at ENTRY is `asm`; `{Name}` in it becomes that import's IAT slot."""
    imports = imports or {}
    probe = make_pe([(".text", b"\x90" * 16, CODE)], imports, 0x1000)
    pe = pefile.PE(data=probe)
    slots = {}
    for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for i in e.imports:
            slots[i.name.decode()] = i.address
    code = asm32(asm.format(**{k: hex(v) for k, v in slots.items()}))
    assert len(code) < 0x1000
    return make_pe([(".text", code, CODE)], imports, 0x1000)


def test_stdcall_import_called_and_popped():
    h = Pe32Host(build("mov ebx, esp\ncall dword ptr [{GetVersion}]\nsub ebx, esp\nmov ecx, ebx\nret",
                       {"KERNEL32.dll": ["GetVersion"]}))
    h.api["GetVersion"] = (0, lambda: 0x0A280105)
    assert h.call(ENTRY) == 0x0A280105
    assert h.reg("ecx") == 0


def test_args_reach_handler_in_order():
    h = Pe32Host(build("push 3\npush 20\npush 100\ncall dword ptr [{Add3}]\nret",
                       {"MATH.dll": ["Add3"]}))
    seen = []
    h.api["Add3"] = (3, lambda a, b, c: seen.append((a, b, c)) or a + b + c)
    assert h.call(ENTRY) == 123
    assert seen == [(100, 20, 3)]


def test_cdecl_import_not_popped():
    h = Pe32Host(build("mov ebx, esp\npush 7\npush 5\ncall dword ptr [{Sum}]\nadd esp, 8\n"
                       "sub ebx, esp\nmov ecx, ebx\nret", {"USER32.dll": ["Sum"]}))
    h.api["Sum"] = (None, lambda argp: h.u32(argp) + h.u32(argp + 4))
    assert h.call(ENTRY) == 12
    assert h.reg("ecx") == 0


def test_unimplemented_import_raises_with_name():
    h = Pe32Host(build("call dword ptr [{GetTickCount}]\nret", {"KERNEL32.dll": ["GetTickCount"]}))
    with pytest.raises(Win32Unimplemented, match="GetTickCount"):
        h.call(ENTRY)


def test_fs_points_at_teb():
    h = Pe32Host(build("mov eax, dword ptr fs:[0x18]\nmov ecx, dword ptr fs:[0]\nret"))
    assert h.call(ENTRY) == h.teb
    assert h.reg("ecx") == 0xFFFFFFFF


def test_virtual_alloc_is_64k_aligned():
    h = Pe32Host(build("ret"))
    a, b = h.virtual_alloc(100), h.virtual_alloc(0x10001)
    assert a % 0x10000 == 0 and b % 0x10000 == 0 and b >= a + 0x10000
    _, va = h.api["VirtualAlloc"]
    assert va(a + 0x1110, 0x1000, 0x1000, 4) == a + 0x1000


def test_callback_runs_python():
    h = Pe32Host(build("push 4\npush 3\nmov eax, dword ptr [esp + 12]\ncall eax\nret 4"))
    cb = h.callback(lambda a, b: a * 10 + b, 2)
    assert h.call(ENTRY, cb) == 34


def test_call_passes_args_and_returns():
    h = Pe32Host(build("mov eax, dword ptr [esp + 4]\nadd eax, dword ptr [esp + 8]\nret 8"))
    assert h.call(ENTRY, 40, 2) == 42
