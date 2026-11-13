# HLSL through the lifted d3dx9_25.dll (plan 9)

Facts about the disc's `d3dx9_25.dll` (rebased to `0x08000000`, lifted as the module `d3dx`) as the
engine loads and calls it. Addresses are at the rebased base. Nothing here quotes the DLL's or the
game's strings, shader text or output.

## The decision (plan 9 Rulings 2 and 3)

`white.exe` imports `D3DXCreateEffect`, `D3DXCompileShader` and `D3DXCompileShaderFromFileA` from
`d3dx9_25.dll`, and the boot hands the first of them HLSL text (11,199 bytes, after the front end's
profile dialog). BW2 compiles it with **Microsoft's own compiler**: the `d3dx9_25.dll` of the April 2005
DirectX redistributable on disc 1, the exact DLL the game imports, extracted, rebased, lifted and compiled
on the user's Mac like `white.exe`. It is never committed or shipped; only its catalog of addresses is
committed (`bw2/catalog/d3dx9_25-apr2005.functions.json`). Effects stay plan 5's: the lifted effect
compiler turns HLSL text into fx_2_0 bytes, and plan 5's code builds the `ID3DXEffect` from them.
`D3DXCompileShader`, the fragment linker and `D3DXCreateSphere` hand the game the lifted DLL's own
objects, which reach our device through its vtable.

Rejected:

- **vkd3d-shader 2.1** (LGPL-2.1+, usable as a separate library): no Homebrew formula, so a from-source
  autotools build on the user's Mac, and its fx_2_0 writer stops on every `sampler_state` initializer,
  which the boot's first effect and 25 of the 59 `.sdv` sources use.
- **Our own HLSL compiler and fx_2_0 writer:** a large compiler with no Windows reference to check it
  against.
- **Capture and precompile:** needs a Windows `fxc`, and there is no Windows reference (spec 11).

The CAB is read with `/usr/bin/tar` (macOS's bsdtar, libarchive, BSD licence), which ships with macOS
and reads Microsoft CAB compression: no new dependency and no GPL tool in the Builder.

## The disc's DLL, its rebase and its lift (Tasks 1 and 2)

| Item | Value |
| --- | --- |
| Disc 1 folder | `DIRECTX9/`: `APR200~1.CAB` (x64, 1,348,242 bytes), `APR200~2.CAB` (x86), `APR200~3.CAB` (managed DirectX) |
| `APR200~2.CAB` | 1,079,850 bytes, SHA-256 `1fc43223b767ec0121989e2c07b56b48ccdd2895c3f638068d24d7869049214d` |
| `d3dx9_25.dll` in it | 2,337,488 bytes, dated Mar 18 2005, file version 9.6.168.0, SHA-256 `4c54df27ce84d21b2924e64ff79b13e7876ce85d8e0c9c1d0abd8da73888187a` |
| Layout | PE32, ImageBase `0x400000`, SizeOfImage `0x253000`; `.text` RVA `0x1000`, virtual size `0x20FEC3` (read-only data merged into it); `.data` `0x211000` (`0x304AC`); `.rsrc` (`0x388`); `.reloc` directory `0x243000` (`0xF070` bytes); no TLS, no delay imports, no RTTI; an Authenticode signature (directory 4), which the rebase invalidates and nothing checks |
| Imports | KERNEL32 71, msvcrt 68, GDI32 26, ADVAPI32 3 |
| Exports | 323 |
| Rebase | to `0x08000000` (between `white.exe`'s end `0x2451210` and win32hle's allocations from `0x10000000`); 28,488 HIGHLOW sites, the only relocation type; ImageBase and those sites are the only bytes changed |
| Rebased image | SHA-256 `505a12cf1fa5a7cf1162a27085f7e30036d7bae4f1b666b999777b60a55c2428`, beside the engine as `out/d3dx9_25.dll` |

The catalog (discovery on the rebased image, two `disasm32` rounds, about 61 minutes): seeds from 323
exports, 254 relocation targets in `.data`, 149 vtable-scan methods, 1,076 code-section relocation
targets (Ruling 14), 0 RTTI, 0 gap seeds, and 35 `EXTRA_SEEDS` (Task 4). It holds 14,430 entries
(141 thunks, 3,895 leaves) and covers 1,992,159 of the 2,162,688 code bytes (92.11%). Starts that only
the control flow of a trusted body reaches are folded into aliases (`catclean.alias_cfg_labels`). The
lift is 5,711 functions in 11 shards and one cold shard (`0x0808E0C4`), 101.3 MB of C, 13,294 trap
sites, about 30 s; `out/bw2` grew from 114.8 MB to 143.6 MB (`build-pipeline.md`, plan 9).

## The imports answered (Task 3)

- **msvcrt** (`runtime/bw2/shims_crt.c`, cdecl): 45 of the 68 names. Heap: `malloc`, `calloc`,
  `realloc`, `free`, `operator new`/`delete`, `_strdup`. Strings and ctype: `strchr`, `_stricmp`,
  `memmove`, `wcslen`, `tolower`, `toupper`, `isalnum`, `isalpha`, `isdigit`, `isxdigit`, `isspace`.
  Numbers: `atoi`, `atol`, `atof`, `floor`, `ceil`, `ldexp`, `frexp`, `modf`, `_finite`, `_isnan`,
  `_fpclass`, `_controlfp`, and the x87 helpers `_CIacos`, `_CIasin`, `_CIexp`, `_CIsqrt`, `_CIcosh`,
  `_CIsinh`, `_CItanh`, `_CIpow`, `_CIfmod`. Others: `_initterm`, `setlocale` ("C" only), `_vsnprintf`
  (msvcrt's three-digit exponents), `qsort`, and `_purecall` and `?terminate@@YAXXZ`, which stop with
  `[bw2] FATAL: msvcrt: <name>`. 22 stay unanswered (below); the 68th is the data import `_adjust_fdiv`.
- **Data imports** (`dll.c`, `bw2_bind_resolve`): `_adjust_fdiv` gets one guest dword holding 0. Every
  other documented msvcrt data export stops the load by name if imported.
- **kernel32** (`runtime/bw2/shims_k32.c`): `DisableThreadLibraryCalls` (TRUE) and
  `IsProcessorFeaturePresent` (from `cpu.c`'s `cpuid` bits: features 2, 3, 6, 8, 9 and 10; 3DNow! absent).
  The other 64 answered names were answered before plan 9 (win32hle or BW2); 5 stay unanswered.
- **GDI32:** `CreateDIBSection` only (win32hle's); 25 stay unanswered. **ADVAPI32:** the 3 registry
  calls, win32hle's.
- **d3d9.dll lookups:** `d3d9` is a win32hle system module, `Direct3DShaderValidatorCreate9` gives an
  accept-all validator and `DebugSetMute` is a no-op (below).

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
then draws on and stops at `_BinkOpen@8` (`sub_0093C7B0`), plan 10's layer (below).

## The game's own compile inputs (Task 6)

### `D3DXCompileShader` call sites

`white.exe` calls `D3DXCompileShader` (thunk `0xB523D8`) from two wrappers of the same shape:
`sub_AB31F0` (call `0xAB324B`, then the device wrapper's vertex shader creation) and `sub_AB3130`
(call `0xAB318B`, the pixel shader one). Each takes `(file name, profile, entry)`, calls `sub_AB2EE0`
to load the text and then passes:

| Argument | Value |
| --- | --- |
| `pSrcData`, `SrcDataLen` | the global buffer `0x23F75C8` and the length at `0x23F75C0` that `sub_AB2EE0` reads |
| `pDefines`, `pInclude` | 0, 0 |
| `pFunctionName`, `pProfile` | the wrapper's third and second arguments, string constants at each caller |
| `Flags` | 1 (`D3DXSHADER_DEBUG`) |
| `ppConstantTable` | 0 |

`sub_AB2EE0` builds the file's path in the `EncryptedShaders` folder with the `.caesar3` extension (the
names are given without the folder), reads at most `0x3FFF` bytes into the buffer (every shipped
`.fx.caesar3` is smaller, the largest 12,017 bytes) and subtracts 3 from each byte (the loop at
`0xAB2F95`). No call goes through
`D3DXGetVertexShaderProfile`/`D3DXGetPixelShaderProfile`: every profile is a string constant.

The 13 calls of the two wrappers, with the addresses of the strings they pass (the test pins these as
`<stem>:<entry va>:<profile>`; no game string enters it):

| Call | Wrapper | File | Entry string | Profile |
| --- | --- | --- | --- | --- |
| `0xA17214` | VS | not shipped (name at `0xD0C7B8`) | `0xD0C7D0` | `vs_1_1` |
| `0xA1723A` | PS | not shipped (name at `0xD0C7B8`) | `0xD0C7B0` | `ps_1_1` |
| `0xA574B2` | VS | `IRing` | `0xD0D7A0` | `vs_1_1` |
| `0xA574D6` | VS | `IRing` | `0xD0D788` | `vs_1_1` |
| `0xA62A7D` | VS | `EZRoad` | `0xD0C7D0` | `vs_2_0` |
| `0xA62AA2` | PS | `EZRoad` | `0xD0C7B0` | `ps_2_0` |
| `0xA62B43` | VS | `EZRoad` | `0xD0D92C` | `vs_1_1` |
| `0xA62B68` | PS | `EZRoad` | `0xD0D91C` | `ps_1_1` |
| `0xA8F9DB` | PS | `Windowlight` | `0xD0C7B0` | `ps_1_1` |
| `0xA8F9FF` | VS | `Windowlight` | `0xD0C7D0` | `vs_1_1` |
| `0xA9F9EA` | VS | `Insect` | `0xD0F6BC` | `vs_1_1` |
| `0xAA0A65` | VS | `Decay` | `0xD0C7D0` | `vs_2_0` |
| `0xAA0A8A` | PS | `Decay` | `0xD0C7B0` | `ps_2_0` |

The first two name a file the game tree does not hold; on Windows the read gives 0 bytes there and the
compile fails, so that pair is not in the test.

`RoadCobblesOldSystem` and `ZRock` are named by no code: their names occur in no game binary (ASCII or
UTF-16; `Everything.stuff` holds only a texture of a similar name). The game never compiles them.
Their own functions carry the two entry names the other callers pass at `0xD0C7D0` (vertex) and
`0xD0C7B0` (pixel), so the test compiles each with those entries and the flags above:
`RoadCobblesOldSystem` at `vs_2_0`/`ps_2_0` (its pixel shader at `ps_1_1` fails with `E_FAIL`, error
`X4502` on line 24: it needs a 2.0 model, like `EZRoad`'s high-end pair) and `ZRock` at `vs_1_1`/`ps_1_1`
(it compiles at 2_0 as well).

### `fxsrc_compile`: the 15 compiles

Check `fxsrc_compile` decodes each file in memory (minus 3 per byte), calls the lifted
`D3DXCompileShader` with the entry string read from the mapped `white.exe` and the arguments above, and
decodes the result with `sm_decode`. All 15 give `S_OK`, the version token of their profile and a
shader `sm_decode` accepts; no trap, no unanswered import and no `[module]` line on the way.

| File | Profiles (tokens) |
| --- | --- |
| `Decay` | `vs_2_0` (229), `ps_2_0` (609) |
| `EZRoad` | `vs_2_0` (426), `ps_2_0` (1,240), `vs_1_1` (241), `ps_1_1` (215) |
| `Insect` | `vs_1_1` (1,056) |
| `IRing` | `vs_1_1` (657), `vs_1_1` (687) |
| `Windowlight` | `ps_1_1` (163), `vs_1_1` (226) |
| `RoadCobblesOldSystem` | `vs_2_0` (283), `ps_2_0` (719) |
| `ZRock` | `vs_1_1` (334), `ps_1_1` (122) |

## The oracle: the shipped `.sdv` effects (Task 6)

Check `sdv_oracle` takes each self-contained `.sdv` source (the NUL-ended text at offset 372),
compiles it with the lifted `D3DXCreateEffectCompiler` and `CompileEffect` (no defines, no include,
flags 0), parses our binary and the shipped one (at the first `01 09 FF FE`) with plan 5's `fx_parse`,
and compares them: techniques and passes by name and order, each pass's shader slots and versions,
common parameters' types, order, flags, annotations and values, every state (pass and sampler) by
operation, order and value, shaders with every comment token stripped, and preshaders and state
expressions by their constant table entries, `CLIT`, `PRSI` and `FXLC`. `fx_parse` records the byte
offset of each parameter's record and value and each state's record for this (`fx.h`).

All 15 sources compile with `S_OK`, both binaries parse, and every must-hold item holds: the same
technique and pass names in the same order (`ToneMapping`'s one technique has no pass in either), the
same shader slots and versions in every pass, every parameter in both binaries with the same type and
order (no parameter is in one binary only), the same flags, annotations and values.

| File | Params | Techniques | Passes | Pass shaders | Identical shaders | Version findings (ours/shipped offset of the first) |
| --- | --- | --- | --- | --- | --- | --- |
| `Debug` | 2 | 2 | 2 | 4 | 4 | none |
| `DiscDrawCompose` | 2 | 1 | 1 | 2 | 2 | none |
| `DiscDrawRender` | 2 | 1 | 1 | 2 | 2 | none |
| `LowEndSeaWater` | 32 | 2 | 3 | 6 | 1 | `state_value_differs` 3 (`0x2F78`/`0x2F7C`), `shader_body_differs` 5 (`0x2E28`/`0x2E30`) |
| `SeaWater` | 49 | 2 | 1 | 2 | 1 | `state_value_differs` 1 (`0x1CCC`/`0x1CC4`), `shader_body_differs` 1 (`0x1A88`/`0x1A84`) |
| `Sky` | 21 | 4 | 4 | 8 | 8 | none |
| `Sun` | 11 | 3 | 3 | 6 | 6 | none |
| `TexBlendDebugDraw` | 2 | 1 | 1 | 2 | 2 | none |
| `ToneMapping` | 21 | 1 | 0 | 0 | 0 | none |
| `WaterSplash` | 14 | 2 | 2 | 4 | 4 | none |
| `basic_particle` | 14 | 2 | 2 | 2 | 0 | `state_value_differs` 2 (`0xFB8`/`0xFB4`), `shader_body_differs` 2 (`0xDA0`/`0xD9C`) |
| `basic_particle_pixel` | 11 | 3 | 3 | 3 | 3 | none |
| `terrain_depth_cueing` | 9 | 1 | 1 | 2 | 2 | none |
| `terrain_fogging` | 4 | 1 | 1 | 2 | 2 | none |
| `terrain_projected_shadows` | 4 | 1 | 1 | 2 | 2 | none |

39 of the 47 pass shaders are equal token for token once comments are stripped. The 8 that differ and
the 6 preshaders were read at the printed offsets (a one-off token dump, not kept), and every
difference is in the bytes of the binaries, so none is a parser bug:

- **`shader_body_differs`** (all 8): the same instruction count, opcodes, source and destination
  registers, swizzles and `def` values (e.g. the same float bits for every literal); only the constant
  register numbers differ (the two compilers lay out `def` constants and parameters in another order).
  In two `LowEndSeaWater` vertex shaders (`vs_1_1`, 127 and 87 tokens without comments) the 9.06
  compiler also schedules a run of `dp4`/`mul`/`mad`/`mov` instructions in another order, and writes
  `oPos.z` with a `dp4` of its own and then `oPos.xyw` where the 5.04 output writes `oPos.xyzw` at
  once.
- **`state_value_differs`** (all 6 are shader preshaders): the `CLIT` literals are equal double for
  double and the `FXLC` instructions are equal opcode for opcode and input for input; only the output
  constant registers differ, and with them the `PRSI` ranges (the 5.04 compiler sometimes splits the
  outputs into two ranges) and the constant table's register indices. No FPU cause: no literal or
  `def` value differs anywhere, so the x87 folding and `atof` give the 5.04 compiler's values.

The shipped binaries were made by an older compiler: every CTAB creator stamp in the 59 `.sdv` files
(435 stamps) names compiler 5.04.00.3900, while the lifted `d3dx9_25.dll` stamps 9.06.168.0000. The
`fxsrc_compile` items (call addresses in the call-site table above) all give `S_OK` and a version token
equal to their profile (`0xFFFE0101`, `0xFFFE0200`, `0xFFFF0101`, `0xFFFF0200`).

## The boot from the effect to Bink (Task 7)

**The runtime effect.** After the profile dialog the game calls `D3DXCreateEffect` (thunk
`sub_00B5237E`) with our device, 11,199 bytes of HLSL text built in memory (the `ToneMapping.sdv` source
plus a generated vertex entry, a generated pixel entry and one technique), no defines, no include,
flags 0, no pool, and both out pointers. The lifted compiler gives `S_OK`, 5,220 bytes of fx_2_0 and
2 techniques, in 7 to 23 ms (168 ms with `BW2_TRACE_IMPORTS=1`). For the record (the plan's spike,
"Measured" item 3): when the compile fails the game asks once more with the same text (2 calls), then
goes on to the same `_BinkOpen@8` stop.

**Measured** (headless, `BW2_HLSL_LOG=1 BW2_FRAME_LOG=1`, the boot test's profile script, 2026-11-13,
times from the engine's start as seen on its stderr):

| Run | Support folder | First read | Effect | Stop | Effect to stop | Frames before the effect | Frames from the effect to the stop |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | fresh | - | - | - | - | 354 | 980 |
| 2 | run 1's (profile exists) | 2.47 s | 1.18 s | 33.86 s | 32.67 s | 8 | 1,728 |
| 3 | fresh | 2.31 s | 4.59 s | 34.64 s | 30.05 s | 1,019 | 1,099 |
| 4 | fresh | 2.31 s | 4.57 s | 34.63 s | 30.06 s | 1,053 | 991 |
| 5 | fresh, `yes` on all 10 cores | 13.86 s | 21.71 s | 52.01 s | 30.31 s | 204 | 265 |

Run 1 took 37.6 s of wall time to exit 4 (not stamped per line). **The Bink call is driven by time, not
frames:** from the effect to the stop takes 30.05 to 30.31 s on a fresh profile while the frame count
ranges from 265 to 1,099. It is the front end's attract video (below), opened on a timer, so the boot
test asserts only that at least one frame lies between the effect and the stop. With an existing
profile the dialog is not shown and the effect is compiled before the first input read (frame 8).

In every run: exit 4 with `[bw2] reached _BinkOpen@8: owned by a later plan (from guest 0xDEAD0000,
sub_0093C7B0)` as the only later-plan line, no loud line, no frame after the stop, and every frame
from the first read to the stop at least half non-black, except in run 5 two frames (one before the
effect, one after it) that each follow a `[bw2] metal: command buffer failed: ... Impacting
Interactivity` line (11 such lines in that run). The `[module]` lines are the same 11 in every run,
and the same 11 the engine built at `41a8e9f` (before plan 9; it stops at the effect) prints: the
fiber-local storage lookups (`FlsAlloc`, `FlsGetValue`, `FlsSetValue`, `FlsFree`) by the game's CRT and
again by `ScriptLibraryR.dll`'s, `LoadLibraryA("PSApi.dll")`, and the two ntdll lookups
`ZwLockVirtualMemory` and `ZwUnlockVirtualMemory`. The lifted D3DX prints none; it never asks for
`d3d9d.dll` in the boot.

**Imports first seen after the effect** (a `BW2_TRACE_IMPORTS=1` run; 327 distinct names before it,
134 after it): `ID3DXEffect::Release` (30 calls), `IDirect3DDevice9::SetGammaRamp` (2) and
`_BinkOpen@8` (1). After the effect the game opens more of its own `.sdv` effects (the grass, flower
and post-processing filters), asks for three vegetation and flower textures the tree does not hold,
and lists the new profile's saved games (none).

**No disc, volume or driver question** in the whole traced boot: no `GetVolumeInformation*`,
`GetDriveType*`, `GetLogicalDrive*`, `QueryDosDevice*`, `DeviceIoControl`, service-manager or SetupAPI
call, no device path opened, and no tripwire.

**For plan 10.** The stop's caller is `sub_0093C7B0` (through the `_BinkOpen@8` import). Just before
it the game's CRT file open (`sub_00AEF0C8`, a `CreateFileA`) asks for the front end's attract video
in the `Data/Splash` folder; the installed tree does not hold that file (the folder has only the EA
logo video and two splash images), so the open fails with "file not found", and the game then maps a
view (`MapViewOfFile` from `sub_009240C0`) and calls `_BinkOpen@8` all the same. Whether the discs hold
the video, and what `_BinkOpen@8` must answer for a missing file, is plan 10's to find out.
