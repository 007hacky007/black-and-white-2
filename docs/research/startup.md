# The boot through the loading screen to audio (plan 7)

Measured 2026-11-12 on an Apple M1 Pro (macOS 27.0.1), with `out/bw2` relinked by
`python -m bw2.pipeline --build-only` (114,768,072 bytes), on the patched tree, headless:

```
BW2_HEADLESS=1 BW2_SUPPORT=/tmp/s out/bw2 --game gamefiles/game
```

The run gets through the sprite, the 49 fonts, the game's memory font, its script plugin and
its loading screen. It exits with code 4 at its first call into the audio layer:

```
[bw2] reached DirectSound (CoCreateInstance): owned by a later plan (from guest 0xDEAD0000, sub_008B1400)
```

There is no loud line before it: no `TRIPWIRE`, no trap, no unresolved call, no unimplemented
import or method. The game asks no disc, volume or driver question on the way: no
`GetDriveTypeA`, `GetVolumeInformationA`, `GetLogicalDrives`, `QueryDosDeviceA` or
`DeviceIoControl`, no `\\.\` path, and nothing touches `DrvMgt.dll` or `SECDRV.SYS`. Nothing
from the protection VM asked for one either.

`tests/test_boot.py::test_boot_to_audio` pins this, run with `BW2_FRAME_LOG=1`,
`BW2_FONT_LOG=1` and a fresh support folder:
- exit code 4, no loud line, and exactly one stop, at `DirectSound (CoCreateInstance)`;
- the 49 font lines below, in order, with their sources;
- the script plugin's lifted-DLL line with 0 imports unanswered;
- at least one frame that is at least half non-black.

## Time to the stop

| Run | Support folder | Wall time |
|---|---|---|
| first after the relink | fresh | 1.29 s |
| 1 | fresh | 0.385 s |
| 2 | fresh | 0.386 s |
| 3 | old | 0.370 s |
| 4 | old | 0.366 s |

Headless and untraced. With `BW2_FRAME_LOG=1` the same four kinds of run take 0.374-0.389 s.
With `BW2_TRACE_IMPORTS=1` a run takes about 5 s. Plan 5's stop at the sprite came at
0.30-0.31 s (`shaders.md`), so the stretch from the sprite to audio is under 0.1 s.

## The trace

`BW2_TRACE_IMPORTS=1` gives 1,396,565 lines of stderr and 224 distinct import names. From
`D3DXCreateSprite` to the stop there are 131,331 import calls to 107 distinct names. In call
order, the parts that matter:

1. `D3DXCreateSprite` (thunk `0xB5231E`, from `0x93AFF4`), then `CreateVertexBuffer`
   (`sub_00932830`).
2. `D3DXCreateFontA` 1 of 49 (thunk `0xB52318`, from `0x93AB9C`): Courier New.
3. `ID3DXFont::DrawTextA` from `sub_0093ABC0`, a measure (`DT_CALCRECT`).
4. `SetPixelShader`, `SetVertexShader`, `SetFVF` (`sub_00933A90`), `SetFVF` (`sub_009F3A70`),
   `Clear` (`sub_00A25D90`), `CreateVertexBuffer`.
5. Frames 1 and 2: `Clear` (`sub_009A9CC0`) and `Present` (`sub_00931B00`), each.
6. The memory font: `LoadLibraryA("gdi32.dll")`, `GetProcAddress` twice and
   `AddFontMemResourceEx`, all from `sub_0051F530`.
7. `D3DXCreateFontA` 2 to 49: Arial Narrow.
8. `LoadImageA` 3 times (`sub_009B6690`).
9. `LoadLibraryA` of the script plugin (`sub_00864330`), which loads it as a lifted DLL.
10. The message pump, then frames 3 and 4.
11. The animation-library scan: `FindFirstFileA` and 37 `FindNextFileA` (`sub_0091F270`), with
    74 `CoFileTimeToDosDateTime` (`sub_0091DFB0`).
12. Only in a traced run: one more `PeekMessageA` and frame 5.
13. Debug lines through `OutputDebugStringA` (6 from the game's log wrapper `sub_009EEC70`, 1
    from `sub_00871410`), then `CoInitializeEx` and `CoCreateInstance`, both from
    `sub_008B1400`.

## Fonts

`D3DXCreateFontA` is called 49 times. Every call passes Width 0, Weight 400 and MipLevels 1
(the font layer stops loudly on any other Width or MipLevels).

- First: `Courier New`, height -16.
- Then 48 times `Arial Narrow`, in four rounds of the heights -10, -8, -13, -10, -15, -12,
  -20, -16, -31, -24, -46, -36, in that order.

The faces bound (from `BW2_FONT_LOG`):

| Creates | Face | Source |
|---|---|---|
| 1 | Courier New -16 | `/System/Library/Fonts/Supplemental/Courier New.ttf` |
| 2-49 | Arial Narrow, all 48 | `memory` |

`AddFontMemResourceEx` comes after create 1 and its `DrawTextA`, and before create 2. So every
Arial Narrow font binds to the game's own face, and Courier New binds to the Mac's file.

The memory font, added by `sub_0051F530` (called from `0x61150E`):
- 134,188 bytes, a TrueType file (first dword `00 01 00 00`);
- SHA-256 `bd31d0cf2e5a1a3a9074e98ebe4964c641121e9e660f31627614c4acb6c89c1c`;
- family `Arial Narrow`, subfamily `Regular`;
- 658 glyphs (`maxp`), fontRevision 2.0, unitsPerEm 2048 (plan 7, "Measured" item 4).

The engine never writes it to disk. The Mac's own files differ:
- `Arial Narrow.ttf` has 663 glyphs, SHA-256
  `9e1d881c4e43b51868f5e235ea5a12c838e23cc8df6277df1a3c94e158999826`;
- `Courier New.ttf` is 684,624 bytes with 3,151 glyphs, SHA-256
  `c886ca3172d8ce9189976992d145ec090500a4ff516ecb9d4df56b1a78336247`.

## Sprite and font calls

| Call | Count | From |
|---|---|---|
| `D3DXCreateSprite` | 1 | thunk `0xB5231E` |
| any `ID3DXSprite` method | 0 | - |
| `D3DXCreateFontA` | 49 | thunk `0xB52318` |
| `ID3DXFont::DrawTextA` | 1 | `sub_0093ABC0`, on the Courier New font |
| any other `ID3DXFont` method | 0 | - |

The one `DrawTextA` is a measure (`DT_CALCRECT`), so no glyph is drawn and no glyph cache
texture is made. Each font makes its own sprite inside the engine, and the game makes no call on
it. That sprite's state block is made only at its font's first drawing `DrawText`, and none is
reached.

## Frames

The frame log (`BW2_FRAME_LOG=1`) reads each `Present`'s back buffer back and prints only
numbers. An untraced run makes 4 frames. A traced run is slower and makes a fifth, after the
animation-library scan, so the count depends on the run's speed.

| Frame | Size | Mean RGB | Non-black | Calls in the frame |
|---|---|---|---|---|
| 1 | 1024x768 | 0,0,0 | 0 of 786,432 | 2 `Clear` (`sub_00A25D90`, `sub_009A9CC0`), no scene |
| 2 | 1024x768 | 0,0,0 | 0 of 786,432 | 1 `Clear` (`sub_009A9CC0`), no scene |
| 3 | 1024x768 | 215,207,205 | 786,432 of 786,432 | `BeginScene`, 2 draws, `EndScene` |
| 4 | 1024x768 | 215,207,205 | 786,432 of 786,432 | the same |
| 5 (traced only) | 1024x768 | 215,207,205 | 786,432 of 786,432 | the same |

The draws in frames 3 to 5 are all `DrawPrimitive` from `sub_00A4CB20`. Each one is a
triangle strip (type 5) of 2 primitives with FVF `0x144` (XYZRHW, DIFFUSE, TEX1), stride 28
and no shaders. Before each draw the game calls `SetTexture` (`sub_00A4D3A0`), 6
`SetTextureStageState` (`sub_00932470`), `SetVertexShader`, `SetPixelShader`, `SetFVF` and
`SetStreamSource`.
- The first draw of a frame is textured by a 1280x960 texture, the second by a 64x64 one. Both
  are format 22 (`X8R8G8B8`).
- Start vertices are 0 and 4 in frame 3, then 8 and 12 in frame 4.

Inside frame 3, after `BeginScene`, the game makes 3 textures. Each is `D3DXGetImageInfoFromFileInMemory`,
`D3DXCreateTextureFromFileInMemoryEx` (thunks `0xB52312` and `0xB52300`) and `CreateTexture`.
The whole run makes 4 such textures; the first is plan 5's (`shaders.md`).

## Fixed during the bring-up

Plan 7's Task 1 answers, with the addresses they changed:

- **`LoadImageA`** of a missing file. `0x9B6690` asks for `Data\Font0-0.bmp` 3 times, with
  three prefixes. The file is not in the shipped tree, and the answer is now NULL with
  `ERROR_FILE_NOT_FOUND`. The game takes its error path at `0x9B674E` and calls no GDI DC
  function: there is no `CreateCompatibleDC` in the trace. Before, win32hle's placeholder
  bitmap led the game into `CreateCompatibleDC`, which is unimplemented.
- **`LoadLibraryA`** of a name with no extension. `0x864330` loads the script plugin by a name
  with no extension. It now gets `.dll`, so `PlugIns/ScriptLibraryR.dll` maps at `0x10000000`
  (span `0xA9000`), with 144 imports bound and 0 unanswered. The game then takes 3 addresses
  from it (`GetProcAddress`, `sub_00864330`), and `sub_00735D40` takes 43 more. Before, the load
  failed silently and the game called a null entry point at `0x7361C3`.
  - The plugin's CRT asks `GetProcAddress(kernel32.dll, ..)` for the four fiber-local storage
    functions (`FlsAlloc`, `FlsGetValue`, `FlsSetValue`, `FlsFree`). win32hle answers NULL, as
    it does for the exe's CRT at start-up, and both CRTs fall back to TLS.
- **`CoFileTimeToDosDateTime`**: 74 calls from `sub_0091DFB0`, during the scan of
  `Data\Art\binary_anim_libs\*.al` (1 `FindFirstFileA`, 37 `FindNextFileA`).
- **`AddFontMemResourceEx`** and **`RemoveFontMemResourceEx`**: `sub_0051F530` now finds both
  through `GetProcAddress`. Before, `0x51F634` called address 0.
- **`CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`** from `sub_008B1400` is answered. Earlier
  in the boot, `sub_00934F30` calls `CoInitialize` before DirectDraw's `CoCreateInstance`.
- **The audio boundary.** The DirectSound CLSIDs and `LoadLibraryA` of a DLL in `later.c`'s
  list (`dsound.dll` included) exit 4.

The sprite and the font needed no further fix during the bring-up. The boot went from the
sprite to the audio stop on the first run after Task 4.

## Messages and input

The game pumps window messages through the loading screen. In a traced run:

| Call | Count | From |
|---|---|---|
| `PeekMessageA` | 3 | `sub_009B10D0` |
| `PeekMessageA` | 2 | `sub_009B1100` |
| `GetMessageA` | 1 | `sub_009B1100` |
| `TranslateAcceleratorA` | 1 | `sub_009B1100` |
| `TranslateMessage` | 1 | `sub_009B1100` |
| `DispatchMessageA` | 1 | `sub_009B1100` |

These are the counts after the sprite. Over the whole run the pump at `sub_009B1100` runs
twice, once after the window is made (as in `shaders.md`) and once before frame 3, and
`sub_009B10D0` runs before frame 3, before frame 5 and before the audio start.

The game reads no input before the stop: no `GetDeviceData`, `GetDeviceState`,
`GetCursorPos`, `GetAsyncKeyState` or `GetKeyState`. DirectInput is used only for the setup
recorded in plan 5. So plan 6 (input) still has no input read to start from.

## The audio start, for plan 8

`sub_008B1400` starts audio after the game's audio-init debug line (through `sub_009EEC70`) and
one more debug line from `sub_00871410`. Its calls, from plan 7's "Measured" item 9 (a spike
that answered each stop):

1. `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`, now answered;
2. `CoCreateInstance` of `{47D4D946-62E8-11CF-93BC-444553540000}` (`CLSID_DirectSound`). This is
   the stop today;
3. `CoUninitialize`;
4. `GetVersion`;
5. `LoadLibraryA("dsound.dll")`, in `sub_008B9700`. This would now exit 4 too, as `later.c`
   lists `dsound.dll`;
6. `GetProcAddress(dsound.dll, "DllGetClassObject")`.

The exe also names each of these once: `wrap_oal.dll`, `OpenAL32.dll`, `alcOpenDevice`,
`alcCreateContext`, `alcIsExtensionPresent`, `alIsExtensionPresent`, `alGetProcAddress`,
`EAXGet`, `EAXSet` and `EAX3.0`. It does not name `DirectSoundCreate` or `DirectSoundCreate8`.

The game tree has no OpenAL DLL. Its DLLs are `binkw32.dll`, `dbghelp.dll`, `DrvMgt.dll` and
`PlugIns/ScriptLibraryR.dll`. `Audio/` holds 315 files:
- 264 `.ogg`, 29 `.lug`, 15 `.cue`, and one each of `.bin`, `.dat`, `.h`, `.met`, `.mid` and
  `.txt`, plus one file without an extension;
- 283 of them are under `Music/`, 26 under `SFX/` and 4 under `Dialogue/`.

Whether to answer DirectSound, or to make the DirectSound probe fail so that the game takes
an OpenAL path, is plan 8's question.

## For the next plans

- Plan 8 (audio) starts at `sub_008B1400` with the calls above.
- Plan 6 (input) is still deferred. The game reads no input before audio.
- HLSL compilation, the fragment linker and `D3DXCreateSphere` have not been reached. They
  stay loud later-plan stops.
- The glyph cache and the sprite's draws are not used by the boot yet. The first drawing
  `DrawText` comes after the audio stop.
