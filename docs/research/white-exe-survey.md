# white.exe (fan patch 1.42) survey (phase 0 items 5 and 6, header half)

Date: 2026-11-10. Tool: `python -m bw2.pe_survey gamefiles/exe/white.exe`.
SHA-256 `ca745211ac9c63c439493ff14fa27507c4b99d199af4fd2b48577bccfd851db2`.

## Headers

Image base `0x400000`, entry `0xad8fa3`, no TLS callbacks.

| Section | RVA | Virtual size | Raw size | Flags | Entropy |
|---|---|---|---|---|---|
| `.text` | `0x1000` | 8,454,603 | 8,454,603 | `0xe0000020` (RWX code) | 6.59 |
| `.rdata` | `0x812000` | 1,425,557 | 1,425,557 | `0x40000040` | 5.64 |
| `.data` | `0x96f000` | 23,668,884 | 10,608,160 | `0xc0000040` | 7.43 |
| `_PDATA` | `0x2002000` | 115,544 | 115,542 | `0x40000040` | 6.58 |
| `.tls` | `0x201f000` | 9 | 2 | `0xc0000040` | 0 |
| `.rsrc` | `0x2020000` | 29,726 | 29,725 | `0x40000040` | 4.41 |
| `stxt774` | `0x2028000` | 5,044 | 5,044 | `0xe0000020` | 7.54 |
| `.name0` | `0x202a000` | 157,925 | 159,744 | `0xe0000040` | 7.96 |
| `.name1` | `0x2051000` | 528 | 4,096 | `0x42000040` | 1.22 |

Notes:
- `.text` is writable (`0xe0000020`). A SafeDisc unwrap usually leaves that
  behind. Self-modifying code is possible; the 8 bytes past `.text`'s name
  (`.text\0\x14`) are junk in the name field.
- `.data` entropy 7.43 is high for data. Large compressed or packed tables
  are likely; not investigated.
- Relocations: 260 entries in one block for RVA page `0x202a000` (`.name0`).
  Nothing for game code.

## Imports

| DLL | Functions |
|---|---|
| KERNEL32.dll | 171 |
| d3dx9_25.dll | 60 |
| USER32.dll | 59 |
| WINMM.dll | 32 |
| WS2_32.dll | 28 (by ordinal) |
| IMM32.dll | 14 |
| ADVAPI32.dll | 12 |
| dbghelp.dll | 9 |
| binkw32.dll | 6 |
| ole32.dll | 5 |
| GDI32.dll | 4 |
| SHELL32.dll | 3 |
| comdlg32.dll, DDRAW.dll | 2 each |
| d3d9.dll, DINPUT8.dll, SHLWAPI.dll, COMCTL32.dll, NETAPI32.dll | 1 each |

The game also loads, at run time, `.\PlugIns\ScriptLibraryR.dll` (a 32-bit
game DLL; see `official-patches.md`), OpenAL (`OpenAL32.dll`, `wrap_oal.dll`)
and `dsound.dll`.

## C++ and exceptions (item 6)

| Measure | Count | Meaning |
|---|---|---|
| RTTI type descriptors (`.?AV` / `.?AU` in data) | 2,781 | RTTI is on: class names and vtables can be recovered exactly |
| `mov eax, fs:[0]` in code | 2,215 | SEH frame setups |
| EH funclet stubs `mov eax, FuncInfo; jmp 0xad9240` | 1,892 | functions with C++ unwind tables (`__CxxFrameHandler` at `0xad9240`) |
| `__try` frames (`push 0xad9d0c`, `_except_handler3`) | 7 | structured exception handling |
| Direct calls to `_CxxThrowException` (`0xad9b13`, builds the `0xE06D7363` record and calls `RaiseException`) | 271 | C++ `throw` sites |
| `RaiseException` call sites | 3 (`0x8592e8`, `0xad9b41` in `_CxxThrowException`, `0xae1e9a`) | |

**Verdict: needs guest C++ exception handling early.** 271 throw sites and
1,892 functions with unwind tables mean `throw` is part of the engine, not
only of the crash path. Whether a throw happens on a normal campaign path
cannot be decided statically. The runtime plan has to include
`_CxxThrowException` / `__CxxFrameHandler` support (catch dispatch,
destructor unwinding through lifted frames) and `_except_handler3` for the 7
`__try` blocks, rather than stopping on `RaiseException` as NFSU2 and
pcrecomp's win32hle do. Plan for it, and measure throw frequency at first
boot by trapping `0xad9b13`.
