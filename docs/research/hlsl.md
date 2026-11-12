# HLSL through the lifted d3dx9_25.dll (plan 9)

Facts about the disc's `d3dx9_25.dll` (rebased to `0x08000000`, lifted as the module `d3dx`) as the
engine loads and calls it. Addresses are at the rebased base. Nothing here quotes the DLL's or the
game's strings, shader text or output.

## Loading (Task 4)

- `d3dx_lifted_proc` (`runtime/bw2/d3dx/d3dx_lifted.c`) finds `d3dx9_25.dll` beside the engine binary
  (`_NSGetExecutablePath`, then `realpath`), checks ImageBase `0x08000000` and SizeOfImage `0x253000`,
  and opens it with `dll.c`'s `bw2_lifted_dll_open` at the first call that needs it. The range
  `0x08000000` + 16 MB is held from start-up and released just before the mapping.
- The image is registered as `bw2-d3dx9_25-lifted.dll` in the guest folder of its file, so the game's
  `GetModuleHandleA("d3dx9_25.dll")` gives 0 with error 126 before and after the load (the game never
  loads that name; its imports bind to the native D3DX).
- `DllMain` returns TRUE. It calls `DisableThreadLibraryCalls` (`0x08029015`) and reads `_adjust_fdiv`
  (`0x08209E06`), which `bw2_bind_resolve` answers with a guest dword holding 0.
- The loader line reads `[bw2] d3dx9_25.dll: lifted DLL at 0x08000000, 52 imports unanswered`. The count
  is the imports bound to a stub that stops by name when called (win32hle's `hle_is_stub`). The same
  count for `ScriptLibraryR.dll` is 12 (its crash reporter's dbghelp calls, `DebugBreak`,
  `IsDebuggerPresent`, `PeekNamedPipe`, `DeleteObject`); the line printed 0 for it before, because the
  bind counted stubs as answers.

### The 52 unanswered imports

None is reached by the checks of Task 4. Each stops the run by name if called.

| DLL | Names | Why unanswered |
| --- | --- | --- |
| KERNEL32 | `DeleteFileW`, `MoveFileW`, `GetTempFileNameW`, `ExpandEnvironmentStringsA`, `FindResourceW` | not reached from the five entry points ("Measured" item 7) |
| msvcrt | `fopen`, `_wfopen`, `fread`, `fwrite`, `fclose`, `_tempnam`, `sscanf`, `rand`, `srand`, `_ultoa`, `wcstombs`, `iswspace`, `iswalpha`, `iswdigit`, `iswpunct`, `_strtime`, `_strdate`, `_setjmp3`, `longjmp`, `_except_handler3`, `__CxxFrameHandler`, `exit` | Task 3 left them unanswered on purpose (Ruling 11) |
| GDI32 | `GetCurrentObject`, `SetMapMode`, `GetObjectW`, `GetObjectA`, `ExtTextOutW`, `ExtTextOutA`, `MoveToEx`, `CreateCompatibleDC`, `DeleteDC`, `SelectObject`, `DeleteObject`, `SetTextAlign`, `SetBkMode`, `SetBkColor`, `SetTextColor`, `CreateFontIndirectW`, `CreateFontIndirectA`, `GetFontLanguageInfo`, `GetOutlineTextMetricsA`, `GetCharacterPlacementW`, `GetCharacterPlacementA`, `GetGlyphOutlineA`, `GetGlyphOutlineW`, `GetTextMetricsA`, `GetTextMetricsW` | `ID3DXFont` and text meshes; the game's font stays ours (plan 7), and BW2 registers no GDI device-context layer |

## D3DX's questions to d3d9.dll (Ruling 12)

`d3d9` is a win32hle system module (`third_party/pcrecomp/runtime/win32hle/module.c`), so
`GetModuleHandleA("d3d9.dll")` is non-zero and `GetProcAddress` on it answers from the shim tables.

**The shader validator.** Each of the three lookup sites calls `GetModuleHandleA`, then (if that gave 0)
`LoadLibraryA`, then `GetProcAddress(.., "Direct3DShaderValidatorCreate9")`, skipped when the flags hold
`D3DXSHADER_SKIPVALIDATION` (2). Read from the code:

| Call | Arguments after `this` | Call sites |
| --- | --- | --- |
| `Direct3DShaderValidatorCreate9` | 0 (none pushed) | `0x080F4316`, `0x08149B0D`, `0x08165BAD` |
| slot 3 (Begin: callback, user data, flags) | 3 | `0x080F4329`, `0x08149B24`, `0x08165BC3` |
| slot 4 (Instruction: file, line, tokens, count) | 4 | `0x080F181E`, `0x08143976`, `0x08165BD9`, `0x08165C4A`, `0x08165C96` |
| slot 5 (End) | 0 | `0x080F43FD`, `0x0814A4EB`, `0x08165CA2` |
| slot 2 (Release) | 0 | `0x080F443A`, `0x08149AD0`, `0x08165CCE` |

The three Begin calls pass a D3DX callback (`0x080F0CC7`, `0x08143993`, `0x08165AD9`) that the
validator would call with messages; the accept-all validator never calls it. The site in
`sub_08165B62` fails its caller with `E_FAIL` when the lookup or the creation gives 0, so a missing
`d3d9.dll` would not only skip validation there. Slots 0 and 1 (QueryInterface, AddRef) are not called.
The validator's vtable has 10 entries, each its own shim name; slots 6 to 9 stop with
`[bw2] FATAL: shader validator slot <n> (from 0x<caller>)`.

A `ps_2_0` compile with flags 0 through `D3DXCompileShader` (check `d3dx_validator`) creates one
validator and calls Begin once, Instruction 5 times (from `sub_08143905`), End once and Release once,
through the site in `sub_08149A1B`. The engine names a caller by `g_cur_func` (lifted calls push no real
return address), and `sub_0815B4B9` enters `sub_08149A1B` by a tail jump, so Begin, End and Release
are counted under `sub_0815B4B9`. With `D3DXSHADER_SKIPVALIDATION` no validator is made and the shader
is the same: 46 tokens either way, 12 without comments, and only the CTAB's flags dword differs.

**DebugSetMute.** `sub_08029B42` (called from many places, around operations that might print debug
output) looks up `DebugSetMute` in `d3d9.dll` with `GetModuleHandleA` then `LoadLibraryA`, and in
`d3d9d.dll` with `GetModuleHandleA` only (no `LoadLibraryA`, so the debug runtime's absence prints
nothing). It then calls each pointer found as `push arg; call; pop ecx` (`0x08029C12`, `0x08029C21`),
so the shim pops only its return address. A registry DWORD read through `0x08043ADC` (its value name at
`0x08001398`) can turn the calls off.
One `d3dx_validator` run calls `DebugSetMute` 0 times: no checked path reaches `sub_08029B42`. The
shim is answered and counted all the same, so a later path that mutes runs as on a retail machine.

## The CPU dispatch (Ruling 8)

`sub_08043E54` sets `[0x0821129C]` (`0xFFFF` until the first call with a non-zero argument) and copies a
74-dword function table from `0x08211170` (the x87 routines) to `0x08211048`. Then, unless one of two
registry DWORDs (value names at `0x0800520C` and `0x080051FC`) is 1:

| Value | Path | Set at | Feature asked (`sub_08043DA2`) |
| --- | --- | --- | --- |
| 0 | x87 (the default table) | `0x08043E95` | - |
| 1 | 3DNow! (`sub_081F9832` patches the table) | `0x08043F09` | 7, `IsProcessorFeaturePresent` |
| 2 | SSE2 (`sub_08208230`) | `0x08043F22` | 10, D3DX's own `cpuid` probe (`0x08043C63`) |
| 3 | SSE (`sub_08205C5D`) | `0x08043F3B` | 6, `IsProcessorFeaturePresent` |

With `shims_k32.c`'s `IsProcessorFeaturePresent` (3DNow! absent) and `cpu.c`'s `cpuid` (SSE2 present),
the value after the load and one lifted `D3DXMatrixMultiply` is 2: **the SSE2 path**. The product of two
4x4 matrices through it equals the single-precision product exactly.

## Lifter traps on the lifted D3DX's CPU paths

No check of Task 4 hit a trap. The lifted DLL has 13,294 `RECOMP_TRAP` sites, almost all data in
`.text` decoded as code (privileged and BCD instructions) or 3DNow! code (`pf*`, `femms`), which the
SSE2 selection never runs. Five sit on real integer SIMD instructions:

| Site | Instruction | Routine | Path |
| --- | --- | --- | --- |
| `0x0820861D`, `0x08208686` | `packssdw xmm, xmm` | `sub_08208603`, called by `sub_082086B9`, the SSE2 table's entry at `+0xE8` (`0x08208421`), used by `D3DXFloat32To16Array` | SSE2 (selected) |
| `0x08205DFF`, `0x08205E91` | `pmaxsw mm, m64` | `sub_08205DCB`, called by the SSE table's entry `sub_08205EFA` | SSE (not selected) |
| `0x081FDA04` | `pmovmskb r32, mm` | `sub_081FD9AB`, the 3DNow! table's | 3DNow! (not selected) |

The SSE2 one is a lifter gap on the selected path: a lifted call of `D3DXFloat32To16Array` (the game's
own calls go to the native D3DX) stops at `0x0820861D` naming `packssdw`. None of the checks reaches it.

## Effects, the fragment linker and the sphere (Task 5)

`D3DXCreateEffect` given data that does not start with the fx_2_0 magic calls the lifted
`D3DXCreateEffectCompiler` (7 arguments) and then `ID3DXEffectCompiler::CompileEffect` (vtable slot 59:
57 `ID3DXBaseEffect` slots, `SetLiteral` 57, `GetLiteral` 58), releases the compiler, and builds the
effect from the buffer's bytes on plan 5's binary path, pool included. `D3DXCreateFragmentLinker`
(3 arguments) and `D3DXCreateSphere` (6) forward to the lifted exports as they are.

Measured with the checks' own texts (`tests/test_hlsl.py`), no trap, no unanswered import called and no
`[module]` line from the lifted DLL:

- the effect with a `sampler_state`, a `float4` initializer and `compile vs_2_0`/`ps_2_0`: 372 bytes of
  text give 1,000 bytes of fx_2_0, 1 technique and 3 parameters; its pass sets our device's shaders,
  `ZEnable` and sampler 0's filter and address, and `End` puts them back. The `stateblock_state` form
  (442 bytes, 1,048 bytes of fx_2_0) needs nothing new in plan 5's parser or states.
- a syntax error on line 3 gives `E_FAIL` (`0x80004005`) and an error buffer naming the line as
  `(3)`: this compiler gives an `X3000` syntax error its line alone, not `(line,col)`. A second call
  gives the same HRESULT and text, and with no `ppErrors` the lifted buffer is released.
- `D3DXGatherFragments` of two `vs_1_1` fragments, `AddFragments` and `LinkVertexShader` give an
  `IDirect3DVertexShader9` of our device (the linker's `GetNumberOfFragments`, slot 4, says 2).
- `D3DXCreateSphere(1.0, 8 slices, 4 stacks)`: 26 vertices, 48 faces, FVF `0x12`
  (`XYZ | NORMAL`), its vertex buffer ours, and `DrawSubset(0)` one `DrawIndexedPrimitive` on our
  device (`d3d_draw_calls`, `d3d9_draw.c`).

The boot's own effect (the profile run, `BW2_HLSL_LOG=1`): 11,199 bytes of text give `S_OK`, 5,220
bytes of fx_2_0 and **2 techniques** (the `ToneMapping.sdv` source the text carries holds one
`technique` of its own, line 423 of that source, besides the generated one), in 8-25 ms. The boot
then draws on and stops at `_BinkOpen@8` (`sub_0093C7B0`), plan 10's layer; Task 7 measures that
stretch.
