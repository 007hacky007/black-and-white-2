"""Run a 32-bit Windows DLL in Unicorn, with Python standing in for its imports.

Used to run patchw32.dll (the RTPatch applier shipped inside the official
patches) as is. Each import's IAT slot points at a stub; a code hook on the
stub region runs the Python handler registered in `Pe32Host.api` and returns
to the caller. An import with no handler stops the run with its name.
"""
import struct
from pathlib import Path
from typing import Callable

import pefile
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32, Uc, UcError
from unicorn import x86_const as X


class Win32Unimplemented(Exception):
    """The guest called an import that has no handler."""


class GuestException(Exception):
    """A guest exception that no SEH frame handled."""


STUBS, STUBS_SIZE = 0x7F000000, 0x100000
STOP = STUBS + STUBS_SIZE - 0x1000
STACK, STACK_SIZE = 0x7E000000, 0x00400000
HEAP, HEAP_SIZE = 0x20000000, 0x30000000
TEB, GDT = 0x7FFD0000, 0x7FFE0000

_REGS = {n: getattr(X, f"UC_X86_REG_{n.upper()}") for n in
         ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip", "eflags")}


def _gdt_entry(base: int, limit: int, access: int, flags: int) -> bytes:
    return struct.pack("<Q", (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (access << 40)
                       | (((limit >> 16) & 0xF) << 48) | (flags << 52) | (((base >> 24) & 0xFF) << 56))


class Pe32Host:
    def __init__(self, dll: Path | bytes):
        data = dll if isinstance(dll, (bytes, bytearray)) else Path(dll).read_bytes()
        self.pe = pefile.PE(data=bytes(data))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        size = (self.pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
        if any(self.base < hi and lo < self.base + size for lo, hi in
               ((STACK, STACK + STACK_SIZE), (HEAP, HEAP + HEAP_SIZE), (STUBS, 0x80000000))):
            self.pe.relocate_image(0x60000000)
            self.base = 0x60000000
        self.uc = mu = Uc(UC_ARCH_X86, UC_MODE_32)
        img = self.pe.get_memory_mapped_image()
        mu.mem_map(self.base, size)
        mu.mem_write(self.base, img[:size])
        for lo, n in ((STUBS, STUBS_SIZE), (STACK, STACK_SIZE), (HEAP, HEAP_SIZE),
                      (TEB, 0x10000), (GDT, 0x1000)):
            mu.mem_map(lo, n)
        self.teb = TEB
        self._setup_segments()
        self.api: dict[str, tuple[int | None, Callable]] = {}
        self.last_error = 0
        self.handles: dict[int, object] = {}
        self._next_handle = 0x100
        self._heap_top = HEAP
        self._stub_of: dict[str, int] = {}
        self._name_of: dict[int, str] = {}
        self._next_stub = STUBS
        self._error: BaseException | None = None
        for e in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            for i in e.imports:
                name = i.name.decode() if i.name else f"{e.dll.decode()}#{i.ordinal}"
                self.w32(i.address - self.pe.OPTIONAL_HEADER.ImageBase + self.base, self._stub(name))
        mu.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUBS, end=STUBS + STUBS_SIZE - 1)
        self.api["VirtualAlloc"] = (4, self._virtual_alloc_api)
        self.api["VirtualFree"] = (3, lambda a, n, t: 1)
        self.api["RaiseException"] = (4, self._raise_exception)
        self.api["RtlUnwind"] = (4, self._rtl_unwind)
        self.api["__seh_ret"] = (0, self._seh_ret)
        self._seh_ret_stub = self._stub("__seh_ret")
        self._seh: list[dict] = []

    # ---- set-up ----
    def _setup_segments(self) -> None:
        mu = self.uc
        ents = [b"\0" * 8, _gdt_entry(0, 0xFFFFF, 0x9B, 0xC), _gdt_entry(0, 0xFFFFF, 0x93, 0xC),
                _gdt_entry(TEB, 0xFFF, 0x93, 0x4)]
        mu.mem_write(GDT, b"".join(ents))
        mu.reg_write(X.UC_X86_REG_GDTR, (0, GDT, len(ents) * 8 - 1, 0))
        for r in (X.UC_X86_REG_SS, X.UC_X86_REG_DS, X.UC_X86_REG_ES):
            mu.reg_write(r, 2 << 3)
        mu.reg_write(X.UC_X86_REG_FS, 3 << 3)
        self.w32(TEB, 0xFFFFFFFF)          # end of the SEH chain
        self.w32(TEB + 0x18, TEB)          # NT_TIB.Self

    def _stub(self, name: str) -> int:
        if name in self._stub_of:
            return self._stub_of[name]
        a = self._next_stub
        if a >= STOP:
            raise RuntimeError("out of import stubs")
        self._next_stub += 16
        self.uc.mem_write(a, b"\xc3")
        self._stub_of[name], self._name_of[a] = a, name
        return a

    # ---- memory ----
    def read(self, va: int, n: int) -> bytes:
        return bytes(self.uc.mem_read(va, n))

    def write(self, va: int, data: bytes) -> None:
        self.uc.mem_write(va, bytes(data))

    def u32(self, va: int) -> int:
        return struct.unpack("<I", self.uc.mem_read(va, 4))[0]

    def w32(self, va: int, v: int) -> None:
        self.uc.mem_write(va, struct.pack("<I", v & 0xFFFFFFFF))

    def cstr(self, va: int, wide: bool = False) -> str | None:
        if not va:
            return None
        step, out = (2 if wide else 1), bytearray()
        while True:
            c = self.uc.mem_read(va, step)
            if c == b"\0" * step:
                break
            out += c
            va += step
        return out.decode("utf-16-le" if wide else "cp1252", "replace")

    def put_str(self, va: int, s: str, wide: bool = False) -> int:
        self.write(va, s.encode("utf-16-le") + b"\0\0" if wide else s.encode("cp1252", "replace") + b"\0")
        return len(s)

    def virtual_alloc(self, size: int) -> int:
        a = (self._heap_top + 0xFFFF) & ~0xFFFF
        end = a + ((max(size, 1) + 0xFFFF) & ~0xFFFF)
        if end > HEAP + HEAP_SIZE:
            raise MemoryError("guest heap exhausted")
        self._heap_top = end
        return a

    def _virtual_alloc_api(self, addr: int, size: int, kind: int, prot: int) -> int:
        return addr & ~0xFFF if addr else self.virtual_alloc(size)

    def new_handle(self, obj: object) -> int:
        h = self._next_handle
        self._next_handle += 4
        self.handles[h] = obj
        return h

    def reg(self, name: str) -> int:
        return self.uc.reg_read(_REGS[name])

    def set_reg(self, name: str, v: int) -> None:
        self.uc.reg_write(_REGS[name], v & 0xFFFFFFFF)

    # ---- calls ----
    def export(self, name: str) -> int:
        for s in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if s.name and s.name.decode() == name:
                return self.base + s.address
        raise KeyError(name)

    def callback(self, fn: Callable, nargs: int) -> int:
        name = f"__callback{len(self._stub_of)}"
        self.api[name] = (nargs, fn)
        return self._stub(name)

    def call(self, va: int, *args: int) -> int:
        esp = STACK + STACK_SIZE - 0x1000
        for a in reversed(args):
            esp -= 4
            self.w32(esp, a)
        esp -= 4
        self.w32(esp, STOP)
        self.set_reg("esp", esp)
        self._error = None
        try:
            self.uc.emu_start(va, STOP)
        except UcError as e:
            if self._error is None:
                raise RuntimeError(f"guest fault at {self.reg('eip'):#010x}: {e}") from e
        if self._error is not None:
            err, self._error = self._error, None
            raise err
        return self.reg("eax")

    def _on_stub(self, uc, addr, size, _user) -> None:
        name = self._name_of.get(addr)
        if name is None:
            return
        try:
            if name not in self.api:
                raise Win32Unimplemented(f"unimplemented Win32 import {name} "
                                         f"(called from {self.u32(self.reg('esp')):#010x})")
            nargs, fn = self.api[name]
            esp = self.reg("esp")
            ret = self.u32(esp)
            if nargs is None:                      # cdecl: the caller pops
                r = fn(esp + 4)
                pop = 4
            else:
                r = fn(*(self.u32(esp + 4 + 4 * k) for k in range(nargs)))
                pop = 4 + 4 * nargs
            if r is _DIVERTED:                     # the handler set eip/esp itself
                return
            self.set_reg("eax", r or 0)
            self.set_reg("esp", esp + pop)
            self.set_reg("eip", ret)
        except BaseException as e:                 # Unicorn swallows exceptions in hooks
            self._error = e
            uc.emu_stop()

    # ---- structured exception handling ----
    # Unicorn cannot run a nested emu_start inside a hook, so a handler call is
    # a jump into guest code with __seh_ret as its return address; __seh_ret
    # picks up the dispatch or unwind that made the call.
    _CTX = {"edi": 0x9C, "esi": 0xA0, "ebx": 0xA4, "edx": 0xA8, "ecx": 0xAC, "eax": 0xB0,
            "ebp": 0xB4, "eip": 0xB8, "eflags": 0xC0, "esp": 0xC4}

    def _call_handler(self, st: dict, frame: int) -> object:
        handler = self.u32(frame + 4)
        sp = st["area"]
        for a in (st["disp"], st["ctx"], frame, st["rec"]):
            sp -= 4
            self.w32(sp, a)
        sp -= 4
        self.w32(sp, self._seh_ret_stub)
        st["frame"] = frame
        self.set_reg("esp", sp)
        self.set_reg("eip", handler)
        return _DIVERTED

    def _new_records(self, esp0: int, code: int, flags: int, nparams: int, argv: int,
                     address: int) -> dict:
        area = esp0 - 0x800
        rec, ctx, disp = area, area + 0x100, area + 0x400
        params = [self.u32(argv + 4 * i) for i in range(min(nparams, 15))] if argv else []
        self.write(rec, struct.pack("<IIIII", code, flags, 0, address, len(params))
                   + b"".join(struct.pack("<I", p) for p in params).ljust(60, b"\0"))
        self.write(ctx, b"\0" * 0x2CC)
        self.w32(ctx, 0x10007)
        for r, off in self._CTX.items():
            self.w32(ctx + off, self.reg(r))
        self.w32(disp, 0)
        return {"rec": rec, "ctx": ctx, "disp": disp, "area": area - 0x40}

    def _raise_exception(self, code: int, flags: int, nparams: int, argv: int) -> object:
        esp0 = self.reg("esp")
        st = self._new_records(esp0, code, flags & 1, nparams, argv, self.u32(esp0))
        self.w32(st["ctx"] + self._CTX["eip"], self.u32(esp0))
        self.w32(st["ctx"] + self._CTX["esp"], esp0 + 4 + 16)
        st.update(kind="dispatch", code=code)
        frame = self.u32(self.teb)
        if frame == 0xFFFFFFFF:
            raise GuestException(f"unhandled guest exception {code:#010x}")
        self._seh.append(st)
        return self._call_handler(st, frame)

    def _rtl_unwind(self, target: int, ip: int, rec: int, value: int) -> object:
        esp0 = self.reg("esp")
        self._seh = [s for s in self._seh if s["kind"] != "dispatch"]   # a handler claimed it
        st = self._new_records(esp0, 0xC0000027, 2, 0, 0, self.u32(esp0))
        if rec:
            st["rec"] = rec
            self.w32(rec + 4, self.u32(rec + 4) | 2)                    # EH_UNWINDING
        if target == 0:
            self.w32(st["rec"] + 4, self.u32(st["rec"] + 4) | 4)        # EH_EXIT_UNWIND
        st.update(kind="unwind", target=target or 0xFFFFFFFF, value=value,
                  ret=ip or self.u32(esp0), esp=esp0 + 4 + 16,
                  saved={r: self.reg(r) for r in ("ebx", "esi", "edi", "ebp")})
        return self._unwind_next(st)

    def _unwind_next(self, st: dict) -> object:
        frame = self.u32(self.teb)
        if frame == st["target"] or frame == 0xFFFFFFFF:
            for r, v in st["saved"].items():
                self.set_reg(r, v)
            self.set_reg("eax", st["value"])
            self.set_reg("esp", st["esp"])
            self.set_reg("eip", st["ret"])
            return _DIVERTED
        self._seh.append(st)
        return self._call_handler(st, frame)

    def _seh_ret(self) -> object:
        if not self._seh:
            raise GuestException("SEH handler returned with no dispatch in progress")
        st = self._seh.pop()
        disposition = self.reg("eax")
        if st["kind"] == "unwind":
            self.w32(self.teb, self.u32(st["frame"]))                   # unlink that frame
            return self._unwind_next(st)
        if disposition == 0:                                            # continue execution
            for r, off in self._CTX.items():
                self.set_reg(r, self.u32(st["ctx"] + off))
            return _DIVERTED
        if disposition == 1:                                            # continue search
            nxt = self.u32(st["frame"])
            if nxt == 0xFFFFFFFF:
                raise GuestException(f"unhandled guest exception {st['code']:#010x}")
            self._seh.append(st)
            return self._call_handler(st, nxt)
        raise GuestException(f"SEH handler returned disposition {disposition} "
                             f"for exception {st['code']:#010x}")


_DIVERTED = object()
