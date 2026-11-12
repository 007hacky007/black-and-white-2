# Plan 7: Sprites, fonts and the loading screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game gets past `D3DXCreateSprite` and shows its loading screen. This plan adds:
- `ID3DXSprite` and `ID3DXFont`, with the game's own TrueType font (installed with `AddFontMemResourceEx`) rasterized by CoreText;
- the Win32 and COM answers the boot needs after them;
- a per-frame log that tells a test what each `Present` showed.

The boot then stops at the audio layer, at `CoCreateInstance(CLSID_DirectSound)`.

**Architecture:**
- **Host code calls guest COM.** The sprite and the font reach the device (and the font reaches its sprite) through the guest vtables, as plan 5's effects do. So state blocks, Get calls and draws see what the game would see. A shared helper `d3d_call` replaces the effect layer's private one.
- **The sprite** queues quads and draws them at `Flush`/`End` with `DrawPrimitiveUP`. In screen space it transforms the corners on the CPU and draws pre-transformed vertices, so the device's transforms are left alone.
- **The font** has two parts:
  - `font_ct.m` is a pure host module: faces (memory and system), GDI-style metrics from the font's own tables, and glyph coverage bitmaps from CoreText.
  - `d3dx_font.c` is the COM object: `DrawText` layout, a glyph cache in `A8R8G8B8` textures, and quads drawn through a sprite.
- **The audio stop** is a COM class registered for the DirectSound CLSIDs. Its create function exits 4 as a later plan's layer.
- **Phasing (my call, spec 11 leaves phasing to the builder):** spec 5.2 gave plan 7 the sprite, font, mesh, HLSL compilation and the fragment linker. The boot reaches only the sprite and the font before audio. HLSL, the fragment linker and `D3DXCreateSphere` stay loud later-plan stops until a boot reaches them. Input stays deferred (plan 6): the game pumps messages through the loading screen but reads no input before audio.

**Tech Stack:** C and Objective-C (clang, arm64, ARC, macOS 13+), Metal, CoreText and CoreGraphics, pcrecomp win32hle (vendored, MIT), Python 3.14 and pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 2, 5.2 D3DX and Audio, 7.3). Facts: `docs/research/shaders.md` (where plan 5 stops), `docs/research/d3dx-usage.md`.

## Measured before writing (2026-11-12)

Main at `6fd8422`, `out/bw2` relinked (114,710,776 bytes), headless, fresh support folder:

```
[bw2] reached D3DXCreateSprite: owned by a later plan (from guest 0xDEAD0000, sub_00B5231E)
```

A throwaway spike branch (deleted, nothing kept) answered each stop with the least that let the boot go on, until the boot reached audio. In call order:

1. **`D3DXCreateSprite`** is called through the thunk `0xB5231E`, from `0x93AFF4` (`0x93B76A` is the other site). The game makes one sprite and calls none of its methods before audio.
2. **`D3DXCreateFontA`** (thunk `0xB52318`, from `0x93AB9C`) is called 49 times:
   - once for `"Courier New"` height -16;
   - 48 times for `"Arial Narrow"`: four rounds of the heights -16, -31, -24, -46, -36, -10, -8, -13, -10, -15, -12, -20.

   Every call passes Width 0, Weight 400, MipLevels 1, Italic 0, CharSet 1 (`DEFAULT_CHARSET`), OutputPrecision 7 (`OUT_TT_ONLY_PRECIS`), Quality 4 (`ANTIALIASED_QUALITY`) and PitchAndFamily 0.
3. **One `ID3DXFont::DrawTextA`**, from the game's text wrapper `0x93ABC0`: sprite NULL, text `"X"`, count 1, rect (0,0,0,0), format `0x524` (`DT_CALCRECT | DT_NOCLIP | DT_SINGLELINE | DT_VCENTER`), colour 0. It is a measure. The call came in at vtable slot 14 (`DrawTextA` in d3dx9_25's order) with matching arguments, which confirms the font's order up to there. The wrapper appends `'X'` to text ending in a space before measuring.
4. **The memory font.** `0x51F530` makes these calls:
   - `LoadLibraryA("gdi32.dll")`;
   - `GetProcAddress` of `AddFontMemResourceEx` and `RemoveFontMemResourceEx`. Today neither is answered, so `0x51F634` calls address 0 (`unresolved indirect call at 0x00000000 (in sub_0051F530)`);
   - after decoding a font held in the exe, `AddFontMemResourceEx(data, 134,188, NULL, &count)`.

   The data is a TrueType file (`00 01 00 00`), SHA-256 `bd31d0cf2e5a1a3a9074e98ebe4964c641121e9e660f31627614c4acb6c89c1c`. Its family is `Arial Narrow`, subfamily `Regular`. Its numbers:
   - unitsPerEm 2048, usWinAscent 1888, usWinDescent 431, xAvgCharWidth 741;
   - it has `hdmx`, `VDMX`, `LTSH`, `kern` and `gasp` tables.

   macOS also ships `Arial Narrow` and `Courier New` (`/System/Library/Fonts/Supplemental/`).
5. **Bitmaps from files.** `0x9B6690` tries `LoadImageA(0, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION)` for `Data\Font0-0.bmp` with three prefixes: none, `"%c:\"` with the letter at `0x176EE08`, and `..\`. The letter is `'D'`: static data no code writes, so this is not a drive query. The file is not in the shipped tree.

   win32hle answers a missing file with its placeholder bitmap `0x1C000003`, not NULL. The game then goes on to `CreateCompatibleDC`, which is unimplemented, and stops. With NULL (as Windows gives) the game takes its error path (`0x9B674E`) and needs no GDI DC at all.
6. **`LoadLibraryA(".\PlugIns\ScriptLibraryR")`**: the name has no extension (`0x864330`, name built at `0x864304`). Windows appends `.dll`. win32hle does not, so the lifted-DLL hook finds no file and the load fails **silently**. The game then calls the plugin's null entry points at `0x7361C3` (`unresolved indirect call at 0x00000000 (in sub_007361C0)`). With `.dll` appended, the lifted DLL loads at `0x10000000` with 0 imports unanswered, and the game prints its "Register types.." and "InitOneTimeOnly" lines.
7. **`CoFileTimeToDosDateTime`** (ole32, from `0x91DFB0`) is called 74 times while the game scans `Data\Art\binary_anim_libs\*.al` (37 `FindNextFileA`). win32hle does not answer it.
8. **The loading screen.** The game makes five `Present`s before audio, read back by a spike frame log:
   - frames 1-2: `Clear` only, all black;
   - frames 3-5: 1024x768, every pixel non-black, mean RGB about (215, 207, 205). Each has `BeginScene`, two textured `DrawPrimitive` calls (textures from `D3DXCreateTextureFromFileInMemoryEx`, 3 made, read through the streaming reader) and `EndScene`.

   Between frames the game pumps messages: `PeekMessageA`, `GetMessageA`, `TranslateAcceleratorA`, `TranslateMessage`, `DispatchMessageA`. It makes **no input read**: no `GetDeviceData`, `GetDeviceState`, `GetCursorPos`, `GetAsyncKeyState` or `GetKeyState`.
9. **Audio** starts with the game's "InitialiseAudio" line, from `sub_008B1400`:
   1. `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`, which win32hle does not answer;
   2. `CoCreateInstance({47D4D946-62E8-11CF-93BC-444553540000})` = `CLSID_DirectSound`, which fails today with "no such class";
   3. `CoUninitialize`;
   4. `GetVersion`;
   5. `LoadLibraryA("dsound.dll")` (`sub_008B9700`). win32hle answers it as a system module (`0x7E0B0000`) although `later.c` lists `dsound.dll`;
   6. `GetProcAddress(dsound.dll, DllGetClassObject)`, not answered, and a call to address 0.

   The exe also names `wrap_oal.dll`, `OpenAL32.dll`, `alcOpenDevice` and `EAXGet`/`EAXSet`. The game tree has no OpenAL DLL. Which audio API to answer is plan 8's question.
10. No `TRIPWIRE`, no disc or volume query (`GetDriveTypeA`, `GetVolumeInformationA`, `DeviceIoControl`: none), and nothing from the protection VM on the way. In the spike, the stretch from the sprite to audio took 0.37-0.58 s of wall time headless. The whole traced run was 1,396,446 lines with 225 distinct imports.
11. Between the sprite and audio the D3DX calls are: `D3DXCreateFontA` 49, `D3DXCreateTextureFromFileInMemoryEx` 4, `D3DXGetImageInfoFromFileInMemory` 4. No `D3DXCreateSphere`, HLSL compile, fragment linker, sprite method or `ID3DXFont` method other than the one `DrawTextA`.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings or bytes copied from them, fonts, textures, frames, or generated C. Facts only (addresses, sizes, counts, hashes). The game's memory font is never written to disk by the engine or a test. Font tests use the Mac's own fonts, or a renamed copy written to the test's `tmp_path`. Run `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The tripwire stays on, and its allow list (`runtime/bw2/guard.c` `g_allowed`) stays empty. Winsock keeps answering "no network".
- Fail loudly, never return 0 silently:
  - An unimplemented method stops with `d3d9: <Interface>::<Method> not implemented (from guest 0x%08X)`, as plan 4's do.
  - A sprite flag, a `DrawText` format bit, a font argument or a font face this plan does not handle stops loudly, naming the value. The exact lists are in Tasks 2-4.
- **Later plans' layers** each exit 4 with `[bw2] reached <name>: owned by a later plan`:
  - `D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXCreateFragmentLinker`, `D3DXCreateSphere`, and `D3DXCreateEffect (HLSL source)` stay as they are.
  - `D3DXCreateSprite` and `D3DXCreateFontA` leave `later.c`.
  - New stops: `DirectSound (CoCreateInstance)` for the DirectSound CLSIDs, and `LoadLibraryA` of any DLL in `later.c`'s list (`dsound.dll` included), even where win32hle knows it as a system DLL. Bink stays as in plan 3.
- **`bw2.h` does not change.** Every lifted shard includes it, so a change rebuilds the whole game (about 11 minutes). A new cross-file function is declared `extern` in the file that uses it, as `register.c` does. Headers under `runtime/bw2/d3d9/` and `runtime/bw2/d3dx/` may change; each change costs a 2-minute relink.
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES`. The line gives the path relative to `third_party/pcrecomp`, then what changed and why. No NFSU2Recomp code. The sprite and font follow the D3DX and GDI documentation and the TrueType specification; no D3DX or Wine code is read into this repo.
- **Fonts (my calls, recorded as rulings):**
  - The rasterizer is CoreText/CoreGraphics. They are Apple system frameworks, so they add no third-party licence. `FRAMEWORKS` gains `"CoreText"`.
  - A memory font is private to the process, as `AddFontMemResourceEx` fonts are on Windows. It is never registered with the system. A memory face wins over a system face of the same name.
  - Metrics follow GDI's rules from the font's `OS/2`, `hhea`, `head` and `hdmx` tables (Task 3), not CoreText's line metrics. Glyph shapes are CoreText's antialiased coverage, with no font smoothing.
- Exit codes are unchanged: 0 normal, 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- Headless (`BW2_HEADLESS=1`) for every test. Pixel tests draw into offscreen targets and read them back.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Identity is 007hacky007. Never push. Work on branch `plan7`.

## Review Focus

- **1:1 sprites are exact.** A sprite drawn at an integer screen position with an identity transform maps texel centres to pixel centres. With `LINEAR` filtering every texel comes out unblurred, and text drawn by the font is as sharp as its glyph bitmap. (Task 2, group `sprite_texel_exact`.)
- **`DT_CALCRECT` then draw.** Text measured with `DT_CALCRECT | DT_WORDBREAK` in a rect, then drawn into the rect it returned, breaks into the same lines at the same glyph positions. Rounding must not move the last word. (Task 4, group `font_calcrect_roundtrip`.)
- **Device reset with sprites and fonts alive:**
  1. `OnLostDevice` on each;
  2. `Reset` succeeds, because neither holds a `D3DPOOL_DEFAULT` resource or a state block;
  3. `OnResetDevice` on each;
  4. text and sprites draw as before.

  (Task 4, group `font_reset`.)
- **Bytes 0x80-0xFF.** `DrawTextA` reads them as Windows-1252: 0x92 is U+2019 and 0xE9 is U+00E9. A character the face lacks draws the face's default glyph (glyph 0) with its advance, never nothing and never a crash. (Task 4, group `font_cp1252`.)
- **A full glyph cache.** The 224 printable characters of a 46-pixel face do not fit one cache texture. They spill into a second one, and every glyph still draws from the right texture and cell. (Task 4, group `font_cache_spill`.)

---

### Task 1: Win32 and COM answers on the way to audio

**Files:**
- Modify (pcrecomp, one commit each with its `third_party/pcrecomp.PATCHES` line):
  - `third_party/pcrecomp/runtime/win32hle/module.c`: `LoadLibraryA`'s default extension;
  - `third_party/pcrecomp/runtime/win32hle/ole32.c`: `CoInitializeEx` and `CoFileTimeToDosDateTime`.
- Modify (BW2):
  - `runtime/bw2/shims_user.c`: `LoadImageA` from a missing file;
  - `runtime/bw2/later.c`: the `LoadLibraryA` stop and the DirectSound class stop;
  - `runtime/bw2/shims_selftest.c`, `tests/test_engine.py` (only if a new group name must be listed).

**Interfaces:**
- Consumes: win32hle's `hle_lifted_dll_hook`, `hle_com_register_class(clsid, create)`, `bw2_hle_shim(name)` (`register.c`), and `bw2_later_dll(base)`.
- Produces:
  - **`LoadLibraryA` (module.c):** the name is normalized once, before any lookup. If the part after the last `\` or `/` has no `.`, `.dll` is appended. A trailing `.` means "no extension": it is removed and nothing is appended. This is Windows' rule. The normalized name is used for `find_by_name`, `system_module`, the host path and the lifted-DLL hook. It is also what a "not found" line prints. A failure through the hook (`r < 0`) now also prints `[module] LoadLibraryA("<name>"): not found`.
  - **`CoInitializeEx(reserved, coinit)` (ole32.c):** reserved must be NULL, else `E_INVALIDARG` (0x80070057). The model is `COINIT_APARTMENTTHREADED` (2) or multithreaded (0); flags 4 and 8 are ignored. It gives `S_OK` on the first call, then `S_FALSE` (1) while initialized. A different model than the first gives `RPC_E_CHANGED_MODE` (0x80010106). `CoInitialize(NULL)` counts as apartment-threaded, and `CoUninitialize` counts calls down.
  - **`CoFileTimeToDosDateTime(FILETIME *ft, WORD *date, WORD *time)` (ole32.c):** no time-zone conversion, as `FileTimeToDosDateTime` does none.
    - date = `(year - 1980) << 9 | month << 5 | day`;
    - time = `hour << 11 | minute << 5 | second / 2`.

    Years before 1980 or after 2107 give FALSE.
  - **`LoadImageA` (shims_user.c, registered before win32hle's):** with `LR_LOADFROMFILE` and type `IMAGE_BITMAP` (0), a path that does not resolve or does not exist gives NULL and `ERROR_FILE_NOT_FOUND` (2). A file that exists goes to win32hle's `LoadImageA`. If win32hle then answers with its placeholder (`0x1C000003`), the run stops loudly: `LoadImageA: "<path>" is not a bitmap win32hle reads`. Every other call goes to win32hle unchanged.
  - **`later.c`:**
    - a `LoadLibraryA` shim, registered first, computes the base name with the rule above. A DLL in `g_later_dlls` gives `[bw2] reached <base>: owned by a later plan` and exit 4. Every other call goes to win32hle's `LoadLibraryA`;
    - `hle_com_register_class` for each DirectSound CLSID, whose create function exits 4 as `DirectSound (CoCreateInstance)`. The CLSIDs:
      - `CLSID_DirectSound` {47D4D946-62E8-11CF-93BC-444553540000};
      - `CLSID_DirectSound8` {3901CC3F-84B5-4FA4-BA35-AA8172B8A09B};
      - `CLSID_DirectSoundCapture` {B0210780-89CD-11D0-AF08-00A0C925CD16};
      - `CLSID_DirectSoundCapture8` {E4BCAC13-7F99-4908-9A8E-74E3BF24B6E1};
      - `CLSID_DirectSoundFullDuplex` {FEA4300C-7959-4147-B26A-2377B9E7A91D}.

- [ ] **Step 1: Write the failing checks** in `runtime/bw2/shims_selftest.c` (run by `tests/test_engine.py::test_shims_selftest`):
  - `LoadLibraryA("kernel32")` returns the same handle as `LoadLibraryA("kernel32.dll")`.
  - With a test `hle_lifted_dll_hook` that records its base name and returns 0: `LoadLibraryA(".\\PlugIns\\Thing")` gives the hook `thing.dll`, `"Thing.bin"` gives `thing.bin`, and `"Thing."` gives `thing`. Restore the hook after.
  - `CoInitializeEx(0, 2)` gives 0, then 1. `CoInitializeEx(0, 0)` then gives 0x80010106. `CoInitializeEx(1, 2)` gives 0x80070057. Two `CoUninitialize` calls follow.
  - `CoFileTimeToDosDateTime` of FILETIME `0x01C5CB3B85E7D800` (2005-10-07 12:34:56) gives TRUE, date `0x3347` and time `0x645C`. `0x01A8E79FE0A45300` (1979-12-31 23:59:58) gives FALSE.
  - `LoadImageA(0, "Data\\NoSuchFile.bmp", 0, 0, 0, 0x2010)` gives 0 and `GetLastError()` 2. A real 2x2 24-bit BMP, written by the check into `<support>/C/`, gives a nonzero handle that `GetObjectA(h, 24, buf)` describes as 2x2.
  - The stops each run in a child (`fork`), checked for exit status 4 and the stop line:
    - `LoadLibraryA("dsound.dll")` prints `reached dsound.dll: owned by a later plan`;
    - `CoCreateInstance(&CLSID_DirectSound, 0, 1, &IID_IUnknown, &out)` prints `reached DirectSound (CoCreateInstance): owned by a later plan`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_engine.py -k shims -v`. Expected: FAIL on each new check.
- [ ] **Step 3: Implement `module.c`'s default extension.** Commit: `git commit -m "Give LoadLibraryA names the default .dll extension"`.
- [ ] **Step 4: Implement `ole32.c`'s two functions.** Commit: `git commit -m "Answer CoInitializeEx and CoFileTimeToDosDateTime"`.
- [ ] **Step 5: Implement the BW2 side** (`shims_user.c`, `later.c`).
- [ ] **Step 6: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_engine.py -q`. Then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: all pass, and the boot still stops at `D3DXCreateSprite`.
- [ ] **Step 7: Commit.** `git commit -m "Answer the Win32 calls between the sprite and audio"`

### Task 2: Host calls into guest COM, and ID3DXSprite

**Files:**
- Create: `runtime/bw2/d3dx/d3dx_sprite.c`, `runtime/bw2/d3dx/ui.h`, `runtime/bw2/d3dx/ui_selftest.c`, `tests/test_ui.py`.
- Modify:
  - `runtime/bw2/d3d9/d3d_com.h` and `d3d_com.c`: `d3d_call`, and kinds `D3DK_SPRITE`, `D3DK_FONT`;
  - `runtime/bw2/d3dx/fx_states.c`: its `dcall` becomes a wrapper over `d3d_call`;
  - `runtime/bw2/d3d9/d3d9_methods.c`: interfaces `ID3DXSprite` and `ID3DXFont`;
  - `runtime/bw2/d3d9/d3d_register.c`: `d3dx_sprite_register`;
  - `runtime/bw2/later.c`: drop `D3DXCreateSprite`.

**Interfaces:**
- Consumes: plan 4's `d3d_new`, `d3d_host`, `d3d_vtable`, `d3d_addref`, `d3d_release`, `hle_alloc`, `hle_call_guest`. Uses the device's own `DrawPrimitiveUP`, `SetRenderState`, `SetTextureStageState`, `SetSamplerState`, `SetTexture`, `SetFVF`, `SetVertexShader`, `SetPixelShader`, `CreateStateBlock`, and `IDirect3DStateBlock9::Capture`/`Apply`.
- Produces:
  - `uint32_t d3d_call(uint32_t obj, const char *iface, const char *method, uint32_t nargs, const uint32_t *args);` in `d3d_com.c`. It calls `method` of `obj` through the object's guest vtable, with `obj` as argument 0 and `nargs` further arguments, and returns eax. The slot comes from `d3d_ifaces` by name, cached per (iface, method). An unknown name stops loudly.
  - `ui.h`:
    - `uint32_t d3dx_sprite_create(uint32_t device);`: a guest `ID3DXSprite`, reference count 1, holding a device reference.
    - The `D3DXSPRITE_*` flags.
    - `uint32_t d3dx_sprite_draw_calls(void);`: how many `DrawPrimitiveUP` calls the sprite layer has made (tests).
  - `ID3DXSprite`, IID {BA0B762D-7D28-43EC-B9DC-2F84443B0614}. Its methods in d3dx9_25 order: `QueryInterface`, `AddRef`, `Release`, `GetDevice`, `GetTransform`, `SetTransform`, `SetWorldViewRH`, `SetWorldViewLH`, `Begin`, `Draw`, `Flush`, `End`, `OnLostDevice`, `OnResetDevice`.
  - `ID3DXFont`'s table entry (used by Task 4), IID {D79DBB70-5F21-4D36-BBC2-FF525C213CDC}. Its methods in order: `QueryInterface`, `AddRef`, `Release`, `GetDevice`, `GetDescA`, `GetDescW`, `GetTextMetricsA`, `GetTextMetricsW`, `GetDC`, `GetGlyphData`, `PreloadCharacters`, `PreloadGlyphs`, `PreloadTextA`, `PreloadTextW`, `DrawTextA`, `DrawTextW`, `OnLostDevice`, `OnResetDevice`.
- **`D3DXCreateSprite(device, &sprite)`:** a NULL argument gives `D3DERR_INVALIDCALL`.
- **`Begin(flags)`:**
  - Flags: `DONOTSAVESTATE` 1, `DONOTMODIFY_RENDERSTATE` 2, `OBJECTSPACE` 4, `ALPHABLEND` 16, `SORT_TEXTURE` 32, `SORT_DEPTH_FRONTTOBACK` 64, `SORT_DEPTH_BACKTOFRONT` 128. `BILLBOARD` (8) and any other bit stop loudly, naming the flag.
  - A second `Begin` before `End` gives `D3DERR_INVALIDCALL`.
  - Without `DONOTSAVESTATE`: at the first `Begin` the sprite creates a `D3DSBT_ALL` state block. Each `Begin` captures it, and `End` applies it.
- **The states a draw sets** (unless `DONOTMODIFY_RENDERSTATE`), from the `ID3DXSprite::Begin` documentation:
  - **Render states:**
    - `ALPHABLENDENABLE` TRUE only with `ALPHABLEND`, else FALSE;
    - `ALPHAFUNC` GREATER, `ALPHAREF` 0, `ALPHATESTENABLE` TRUE only with `ALPHABLEND`;
    - `BLENDOP` ADD, `SRCBLEND` SRCALPHA, `DESTBLEND` INVSRCALPHA, `SEPARATEALPHABLENDENABLE` FALSE;
    - `CLIPPING` TRUE, `CLIPPLANEENABLE` 0, `COLORWRITEENABLE` 0xF, `CULLMODE` NONE;
    - `DIFFUSEMATERIALSOURCE` COLOR1, `FILLMODE` SOLID, `FOGENABLE` FALSE, `RANGEFOGENABLE` FALSE;
    - `INDEXEDVERTEXBLENDENABLE` FALSE, `VERTEXBLEND` 0, `LIGHTING` FALSE, `SHADEMODE` GOURAUD;
    - `SPECULARENABLE` FALSE, `SRGBWRITEENABLE` FALSE, `STENCILENABLE` FALSE, `WRAP0` 0.
  - **Stage 0:** `COLOROP` and `ALPHAOP` MODULATE with `ARG1` TEXTURE and `ARG2` DIFFUSE, `TEXCOORDINDEX` 0, `TEXTURETRANSFORMFLAGS` DISABLE. **Stage 1:** `COLOROP` and `ALPHAOP` DISABLE.
  - **Sampler 0:** `ADDRESSU`/`ADDRESSV` CLAMP, `MAGFILTER`/`MINFILTER`/`MIPFILTER` LINEAR, `MAXMIPLEVEL` 0, `MAXANISOTROPY` 1, `MIPMAPLODBIAS` 0, `SRGBTEXTURE` 0.
  - **Shaders:** vertex and pixel shader NULL. The FVF is set as below.
- **`Draw(texture, srcRect, center, position, color)`:**
  - The quad is srcRect (NULL: the whole level 0) from `texture`. Corners are relative to `center` (NULL: 0,0,0), placed at `position` (NULL: 0,0,0), then multiplied by the sprite transform (row vectors: v' = v * M; identity at creation).
  - The queued sprite holds a reference to its texture until it is drawn.
  - Outside `Begin` it gives `D3DERR_INVALIDCALL`. A NULL texture gives `D3DERR_INVALIDCALL`.
- **Screen space (no `OBJECTSPACE`), my call:**
  - the corners are transformed on the CPU and drawn as `D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1` with rhw 1;
  - x and y are moved by -0.5, so texel centres land on D3D9's pixel centres. ID3DXFont text through a sprite is pixel-sharp on Windows, which needs this;
  - the device's transforms are not touched.
- **Object space:** `D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1` vertices in sprite space, drawn with the world transform set to the sprite transform. The view and projection are the device's. Without `DONOTSAVESTATE` the world transform is restored at `End`.
- **`Flush`** draws the queue: a stable sort by texture (`SORT_TEXTURE`), by z ascending (`FRONTTOBACK`) or by z descending (`BACKTOFRONT`). There is one `DrawPrimitiveUP(D3DPT_TRIANGLELIST)` per run of equal textures. **`End`** flushes and then restores. `Flush`/`End` outside `Begin` give `D3DERR_INVALIDCALL`.
- **`GetTransform`/`SetTransform`** round-trip a `D3DXMATRIX`. **`GetDevice`** AddRefs the device. `SetWorldViewRH`/`SetWorldViewLH` stop loudly (needed only for billboards).
- **`OnLostDevice`** ends an open batch without drawing and releases the state block. **`OnResetDevice`** does nothing. The last `Release` releases the queue's textures, the state block and the device.

- [ ] **Step 1: Write the failing tests.** `tests/test_ui.py` builds `ui_selftest` like `tests/test_effects.py`'s fixture and runs groups (`ui_selftest <group>`, output `<group>: 0 failed`). Pixel groups draw into a 64x64 offscreen `A8R8G8B8` target, tolerance 1/255:
  - `d3d_call`: `d3d_call(dev, "IDirect3DDevice9", "SetRenderState", 2, {D3DRS_ZENABLE, 0})` then `GetRenderState` through `d3d_call` gives 0. An unknown method name stops loudly (checked in a child).
  - `sprite_basic`: `Begin(ALPHABLEND)`, then `Draw` of a 4x4 texture of four solid colours at (10,10) and `End`. The 4x4 block at (10,10) has those colours, and the pixels around it keep the clear colour.
  - `sprite_texel_exact`: a 16x16 texture with every texel distinct, drawn at (8,8) with `LINEAR` (the sprite's own filter). Every pixel equals its texel exactly.
  - `sprite_args`: `srcRect` (2,2,4,4) draws only those texels. `center` (2,2,0) shifts the quad by -2,-2. `SetTransform` with scale 2 and translate (20,0) draws the 4x4 texture as 8x8 at (20 + 2*pos.x, 2*pos.y). Colour 0x80FF0000 modulates and blends to half-red over black.
  - `sprite_sort`: three quads drawn with textures A, B, A in that order. With `SORT_TEXTURE`, `d3dx_sprite_draw_calls()` goes up by 2. Without it, by 3. With `SORT_DEPTH_BACKTOFRONT`, the overlapping pixel shows the quad with the smallest z.
  - `sprite_state`:
    - with no flags, every render, stage and sampler state above, the FVF and the shaders are back after `End` as set before `Begin` (a vs_1_1 and a ps_2_0 bound before);
    - with `DONOTSAVESTATE`, `GetRenderState(ALPHABLENDENABLE)` after `End` gives the sprite's value;
    - with `DONOTMODIFY_RENDERSTATE`, a state set by the test before `Begin` is still in force while the sprite draws.
  - `sprite_objectspace`: with `OBJECTSPACE`, a world-space quad under an identity view and an orthographic projection set by the test lands where the projection puts it. The world transform is back after `End`.
  - `sprite_rules`: a second `Begin` gives `D3DERR_INVALIDCALL`, as do `Draw` outside `Begin`, `End` outside `Begin` and a NULL texture. `BILLBOARD` stops loudly naming flag 8, and `SetWorldViewLH` stops as not implemented (both in children). The texture's reference count is +1 while queued and back after `End`.
  - `sprite_lost`: `OnLostDevice` during an open batch draws nothing. `Reset` then succeeds, and after `OnResetDevice` a new batch draws.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_ui.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement `d3d_call`** and move `fx_states.c` onto it. Then run `.venv/bin/pytest tests/test_effects.py -q`. Expected: all pass, as before.
- [ ] **Step 4: Implement the sprite.**
- [ ] **Step 5: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_ui.py tests/test_effects.py tests/test_d3d9.py -q`. Expected: all pass. Then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: the boot now stops at `D3DXCreateFontA`.
- [ ] **Step 6: Commit.** `git commit -m "Add ID3DXSprite"`

### Task 3: Fonts: memory fonts, faces, metrics and glyphs

**Files:**
- Create: `runtime/bw2/d3dx/font.h`, `runtime/bw2/d3dx/font_ct.m` (CoreText), `runtime/bw2/d3dx/font_gdi.c` (the two gdi32 exports).
- Modify: `bw2/build.py` (`FRAMEWORKS` gains `"CoreText"`), `runtime/bw2/d3d9/d3d_register.c`, `runtime/bw2/d3dx/ui_selftest.c`, `tests/test_ui.py`.
- Create: `tests/ttf.py`, a small TrueType table reader and renamer for the tests: `tables(data) -> dict`, `metrics(data, height) -> dict` and `renamed(data, family) -> bytes`.

**Interfaces:**
- Produces (`font.h`):
  - Memory fonts:
    - `uint32_t bw2_font_add_memory(const uint8_t *data, uint32_t len, uint32_t *count);`: copies the data and gives a nonzero handle, with `*count` set to the number of faces, or 0 if it is not a TrueType/OpenType file.
    - `int bw2_font_remove_memory(uint32_t handle);`: 1, or 0 for an unknown handle.
  - Opening a face:
    - `struct bw2_font *bw2_font_open(const char *face, int32_t height, uint32_t weight, int italic, char *err, size_t errlen);`: NULL with a message naming the face, height or style on failure.
    - `void bw2_font_close(struct bw2_font *f);`
  - Metrics:
    - `struct bw2_tm { int32_t height, ascent, descent, internal_leading, external_leading, ave_char_width, max_char_width, weight, overhang, aspect_x, aspect_y; uint8_t first, last, deflt, brk, italic, underlined, struck_out, pitch_family, charset; uint32_t ppem; };`
    - `const struct bw2_tm *bw2_font_metrics(const struct bw2_font *f);`
  - Glyphs:
    - `uint32_t bw2_font_glyph(const struct bw2_font *f, uint32_t codepoint);`: from the face's cmap, 0 when missing;
    - `int32_t bw2_font_advance(const struct bw2_font *f, uint32_t glyph);`: in pixels;
    - `struct bw2_glyph { int32_t left, top; uint32_t w, h; uint8_t *alpha; };`: the black box, with left from the pen and top above the baseline, and w*h coverage bytes (row 0 at the top, malloc'd);
    - `int bw2_font_raster(const struct bw2_font *f, uint32_t glyph, struct bw2_glyph *out);`
  - `uint32_t bw2_cp1252(uint8_t byte);`: the Windows-1252 code point (0x80 U+20AC, ... 0x92 U+2019, ... 0x9F U+0178). The five undefined bytes map to themselves.
  - gdi32 exports, registered with `win32hle_register` so `GetProcAddress(gdi32.dll, ..)` finds them:
    - `AddFontMemResourceEx(data, len, reserved, &count)`: reserved must be NULL, else NULL with `ERROR_INVALID_PARAMETER`;
    - `RemoveFontMemResourceEx(handle)`.
- **Face lookup:**
  1. memory faces by family (name ID 1) or full name (ID 4), case-insensitively, newest first;
  2. then a system face whose CoreText family name equals `face` case-insensitively.
  3. Otherwise it fails: `no font named "<face>"`.

  Weight >= 600 asks for the bold face and `italic` for the italic face. If the family has no such face, it fails naming the style: GDI's synthetic bold and oblique are not done.
- **Metrics (GDI's rules):**
  - `height < 0`: ppem = -height, s = ppem / unitsPerEm, ascent = round(usWinAscent * s), descent = round(usWinDescent * s), height = ascent + descent.
  - `height > 0`: s = height / (usWinAscent + usWinDescent), ppem = round(unitsPerEm * s), ascent = round(usWinAscent * s), descent = height - ascent.
  - `height == 0` fails ("height 0").
  - internal_leading = height - ppem;
  - external_leading = max(0, round((hhea.lineGap - ((usWinAscent + usWinDescent) - (hhea.ascender - hhea.descender))) * s));
  - ave_char_width = round(xAvgCharWidth * s), max_char_width = round(hhea.advanceWidthMax * s);
  - weight is the asked weight (400 for 0), overhang 0, aspect 96 by 96;
  - first = max(usFirstCharIndex - 2, 0), deflt = max(usFirstCharIndex - 1, 0), brk = 0x20, last = min(usLastCharIndex, 0xFF);
  - pitch_family = `TMPF_VECTOR | TMPF_TRUETYPE` (6), plus 1 if `post.isFixedPitch` is 0 (GDI's inverted flag), plus `FF_MODERN` (0x30) when fixed or `FF_SWISS` (0x20) otherwise;
  - charset 0 (ANSI).
- **Advances:** the face's `hdmx` record for ppem when it has one, else round(advanceWidth * s). No kerning, as GDI's `DrawText` uses none.
- **Coverage:** CoreText at size ppem, into an 8-bit gray `CGBitmapContext`, antialiased, with font smoothing off. The black box is the glyph's CoreText bounding rect rounded out to whole pixels. A blank glyph (the space) gives w = h = 0.

- [ ] **Step 1: Write the failing tests** (groups in `ui_selftest`, driven by `tests/test_ui.py`). They use `/System/Library/Fonts/Supplemental/Arial Narrow.ttf` and `Courier New.ttf`; a test skips if one is missing:
  - `font_metrics`: for heights -8, -16, -46 and +20 of the Arial Narrow file, the selftest prints its `bw2_tm` fields as `key=value` lines. They must equal `ttf.metrics(data, height)`, computed in Python from the same file by the rules above. Courier New -16 also gets `pitch_family` 0x36 and must equal the Python result.
  - `font_advance`: for 'A', 'W', 'i', ' ' at -16, `bw2_font_advance` equals the `hdmx` value, or round(advance * s) where there is no record, as `ttf.py` reads it.
  - `font_memory`:
    1. `ttf.renamed(data, "BW2 Test Face")` is written to `tmp_path`; the selftest loads it into guest memory and calls `AddFontMemResourceEx` through `GetProcAddress(LoadLibraryA("gdi32.dll"), ..)`. That gives a handle and count 1.
    2. `bw2_font_open("BW2 Test Face", -16, 400, 0)` succeeds.
    3. `RemoveFontMemResourceEx` then makes it fail with `no font named "BW2 Test Face"`.
    4. A face renamed `"Courier New"` and added from memory is chosen over the system's: its glyph count, from `maxp` in Python, is what the selftest reports.
  - `font_errors`: `"No Such Face"` fails naming it. Arial Narrow weight 700 succeeds (macOS has the bold face). The renamed memory face with weight 700 fails naming the bold style. Height 0 fails. `AddFontMemResourceEx` of 16 zero bytes gives NULL.
  - `font_raster`: 'I' at -46 has a black box about one stem wide and between 0.6 and 0.8 of ppem tall, with the top within 2 pixels of the cap height (OS/2 `sCapHeight` * s, from Python). Its coverage has full (255) pixels in the stem. ' ' gives w = h = 0. 'g' has a negative bottom (top - h < 0).
  - `font_cp1252`: `bw2_cp1252` of 0x41 is 0x41, 0x80 is 0x20AC, 0x92 is 0x2019, 0x9F is 0x0178, 0xE9 is 0xE9 and 0x81 is 0x81.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_ui.py -k font -v`. Expected: FAIL.
- [ ] **Step 3: Implement** `font_ct.m`, `font_gdi.c` and `tests/ttf.py`.
- [ ] **Step 4: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_ui.py -q`. Expected: all pass.
- [ ] **Step 5: Audit and commit.** `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Add memory fonts, GDI metrics and CoreText glyphs"`

### Task 4: ID3DXFont

**Files:**
- Create: `runtime/bw2/d3dx/d3dx_font.c`.
- Modify: `runtime/bw2/d3dx/ui.h`, `runtime/bw2/d3d9/d3d_register.c`, `runtime/bw2/later.c` (drop `D3DXCreateFontA`), `runtime/bw2/d3dx/ui_selftest.c`, `tests/test_ui.py`.

**Interfaces:**
- Consumes: Task 2's `d3d_call`, `d3dx_sprite_create` and the `ID3DXSprite` methods through the guest vtable. Task 3's `bw2_font_*` and `bw2_cp1252`. The device's `CreateTexture` and `IDirect3DTexture9::LockRect`/`UnlockRect` through `d3d_call`.
- Produces: `D3DXCreateFontA(device, Height, Width, Weight, MipLevels, Italic, CharSet, OutputPrecision, Quality, PitchAndFamily, pFaceName, &font)`, 12 arguments. Its rules:
  - Width other than 0 stops loudly. MipLevels other than 1 stops loudly (the game passes 1).
  - CharSet `ANSI_CHARSET` (0) or `DEFAULT_CHARSET` (1); others stop loudly.
  - OutputPrecision, Quality and PitchAndFamily are kept for `GetDesc` and otherwise ignored. Glyphs are always antialiased.
  - A face `bw2_font_open` refuses stops loudly with its message: `D3DXCreateFontA: no font named "<face>"`. My call: the only faces the game asks for exist on every macOS 13, so a missing one is a broken install, not a case to substitute for.
  - The font holds the device, its own sprite (`d3dx_sprite_create`) and a guest scratch block for the arguments it passes to sprite methods.
- Produces for tests (`ui.h`): `struct d3dx_glyph_pos { uint32_t glyph; int32_t x, y; };` and `uint32_t d3dx_font_last_layout(struct d3dx_glyph_pos *out, uint32_t max);`, the glyphs the last `DrawText` placed (also under `DT_CALCRECT`), in order, each with the top-left of its quad in target pixels.
- **Methods:**
  - `GetDevice` (AddRef'd).
  - `GetDescA` fills `D3DXFONT_DESCA`, 56 bytes: Height, Width, Weight, MipLevels, Italic as given; then the CharSet, OutputPrecision, Quality and PitchAndFamily bytes; then FaceName[32]. `GetDescW` fills the same with a WCHAR face (88 bytes).
  - `GetTextMetricsA` fills `TEXTMETRICA` (56 bytes: 11 LONGs, then 9 BYTEs) from `bw2_tm`, and `GetTextMetricsW` fills `TEXTMETRICW` (60 bytes: the four char fields as WCHARs).
  - `PreloadCharacters(first, last)`, `PreloadTextA(text, count)` and `PreloadTextW` put glyphs in the cache.
  - `DrawTextA(sprite, text, count, rect, format, color)` and `DrawTextW`.
  - `OnLostDevice` calls its sprite's, and `OnResetDevice` likewise.
  - `GetDC`, `GetGlyphData` and `PreloadGlyphs` stop as not implemented. The game does not call them, and GDI DCs are not linked.
- **The glyph cache:**
  - 256x256 `D3DFMT_A8R8G8B8` textures in `D3DPOOL_MANAGED`, one level. Each holds a glyph as (255, 255, 255, coverage).
  - Cells are packed in rows (shelf packing) with a 1-pixel empty border, so linear filtering reads no neighbour.
  - A new texture starts when one is full. A glyph larger than 254x254 stops loudly.
  - A glyph is looked up by glyph index.
- **`DrawText` layout** (the documented `ID3DXFont::DrawText` formats only):
  - Formats: `DT_TOP` 0, `DT_LEFT` 0, `DT_CENTER` 1, `DT_RIGHT` 2, `DT_VCENTER` 4, `DT_BOTTOM` 8, `DT_WORDBREAK` 0x10, `DT_SINGLELINE` 0x20, `DT_EXPANDTABS` 0x40, `DT_NOCLIP` 0x100, `DT_CALCRECT` 0x400. Any other bit stops loudly, naming it (`DT_RTLREADING` 0x20000 included).
  - The text: count -1 means NUL-terminated, and count 0 returns 0 and draws nothing. `DrawTextA` bytes map through `bw2_cp1252`. A NULL rect means (0,0,0,0) with `DT_NOCLIP`.
  - **Lines:** `\r\n`, `\n` and a lone `\r` end a line, unless `DT_SINGLELINE`; then CR and LF have no width and draw nothing (my call). With `DT_WORDBREAK`, a line breaks at the last space before the glyph that would pass the rect's width. The spaces at a break take no width. A single word wider than the rect is not split. `DT_EXPANDTABS` sets tab stops every 8 * ave_char_width from the rect's left. Without it, a tab is drawn as its glyph.
  - **Placement:**
    - line height = tm height; the baseline is at line top + ascent;
    - x per glyph: the pen plus the glyph's left, and the pen moves by `bw2_font_advance`;
    - `DT_CENTER`: x0 = left + floor((width - line width) / 2); `DT_RIGHT`: right - line width;
    - `DT_VCENTER` and `DT_BOTTOM` apply only with `DT_SINGLELINE`: top + floor((rect height - tm height) / 2), or bottom - tm height.
  - **Result:** the text's height (lines * tm height). With `DT_VCENTER` or `DT_BOTTOM`, the offset from rect top to the bottom of the drawn text.
  - **`DT_CALCRECT`** sets right = left + widest line and bottom = top + the text's height, after wrapping to the given width with `DT_WORDBREAK`, and draws nothing.
  - **Clipping:** without `DT_NOCLIP`, a glyph quad is cut to the rect by shrinking its source rect and moving its position.
- **Drawing:**
  - With a sprite given, the glyphs go to it, inside the game's `Begin`/`End`, and take its transform.
  - With NULL, the font's own sprite does `Begin(D3DXSPRITE_ALPHABLEND | D3DXSPRITE_SORT_TEXTURE)`, the glyphs and `End`.
  - Each glyph is one sprite `Draw(cache texture, cell rect, NULL, (x, y, 0), color)`.

- [ ] **Step 1: Write the failing tests** (pixel tests on a 256x64 offscreen target, using the Arial Narrow file through `AddFontMemResourceEx` renamed as in Task 3, so the memory path is exercised):
  - `font_create`: the measured call (Height -16, Width 0, Weight 400, MipLevels 1, Italic 0, CharSet 1, OutputPrecision 7, Quality 4, PitchAndFamily 0) succeeds. `GetDescA` gives the arguments back. `GetTextMetricsA` equals Task 3's metrics in `TEXTMETRICA` layout, and `GetTextMetricsW` gives the same numbers. Width 5, MipLevels 2, CharSet 128 and the face `"No Such Face"` each stop loudly (children).
  - `font_measure`: the game's call `DrawTextA(NULL, "X", 1, &r(0,0,0,0), 0x524, 0)` gives r = (0, 0, advance('X'), tm height). It returns tm height. Nothing is drawn: the target is unchanged and no texture is created.
  - `font_draw`: `"Hi"` in white at rect (4,4,200,40) with `DT_NOCLIP`. The pixels under each glyph's black box equal its coverage (alpha-blended white over black, within 2/255), and the pixels outside every black box stay black. The colour 0xFF00FF00 draws green.
  - `font_align`: the measured line width puts `DT_CENTER` and `DT_RIGHT` text at the computed x. `DT_VCENTER | DT_SINGLELINE` in a 64-pixel-tall rect gives top floor((64 - h) / 2). `DT_BOTTOM` returns the documented offset.
  - `font_wrap`: `"aaa bbb ccc"` with `DT_WORDBREAK` in a rect as wide as `"aaa bbb"` gives 2 lines (return 2 * h). A rect narrower than `"aaa"` gives 3 lines, and the word is not split. `"a\nb"` gives 2 lines, or 1 with `DT_SINGLELINE`. A tab with `DT_EXPANDTABS` moves to 8 * ave_char_width.
  - `font_calcrect_roundtrip`: for 5 strings and widths chosen by the test, `DT_CALCRECT | DT_WORDBREAK` gives a rect. Drawing into that rect gives the same glyph x/y list (from a test hook `d3dx_font_last_layout`) as drawing into the original rect with `DT_NOCLIP`.
  - `font_clip`: without `DT_NOCLIP`, a string wider than its rect leaves every pixel outside the rect black. With it, the overflow is drawn.
  - `font_sprite`: with a game sprite in `Begin(ALPHABLEND)` and `SetTransform` scale 2, the text draws at twice the size. No `DrawPrimitiveUP` happens until the sprite's `End`.
  - `font_cp1252`: `DrawTextA` of 0x92 and 0xE9 caches the glyphs of U+2019 and U+00E9 (the glyph indexes from `bw2_font_glyph`). A code point the face lacks (U+4E00 through `DrawTextW`) draws glyph 0 with glyph 0's advance.
  - `font_cache_spill`: `PreloadCharacters(0x20, 0xFF)` on a -46 font creates a second cache texture. Then each of the 224 characters drawn alone matches its coverage at its pixel position.
  - `font_reset`: a font and a sprite alive and drawn once, then:
    1. `OnLostDevice` on both;
    2. `Reset` gives `D3D_OK`;
    3. `OnResetDevice` on both;
    4. `font_draw`'s check passes again.
  - `font_loud`: `GetDC`, `GetGlyphData` and `PreloadGlyphs` stop as not implemented. Format 0x20000 stops naming `DT_RTLREADING` (children).
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_ui.py -k font -v`. Expected: the new groups FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_ui.py tests/test_d3d9.py tests/test_d3dx.py tests/test_effects.py -q`. Expected: all pass. Then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: `test_boot_after_effects` fails, because the boot now goes on past the fonts (Task 5 replaces it).
- [ ] **Step 5: Audit and commit.** CLEAN, then `git commit -m "Add ID3DXFont"`

### Task 5: The frame log, and the boot to audio

**Files:**
- Create: `docs/research/startup.md`.
- Modify:
  - `runtime/bw2/d3d9/d3d9_device.c` (the frame log), `runtime/bw2/d3d9/d3d9_selftest.c` and `tests/test_d3d9.py` (group `frame_log`);
  - `tests/test_boot.py`: `test_boot_after_effects` becomes `test_boot_to_audio`;
  - the spec: section 5.2 (a plan 7 note in D3DX, the phasing, and the Audio entry's measured start);
  - `docs/research/shaders.md` ("For the next plans" points to `startup.md`);
  - `docs/superpowers/plans/2026-11-11-bw2-plan6-input.md`: its status line says it is still deferred after plan 7, because the game pumps messages through the loading screen but reads no input before audio.

**Interfaces:**
- Consumes everything above.
- Produces:
  - **The frame log.** With `BW2_FRAME_LOG=1` (read once), each `Present` reads the back buffer back and prints one line to stderr. No pixels are stored. The line:

    ```
    [bw2] frame <n>: <w>x<h>, mean <r>,<g>,<b>, nonblack <k> of <w*h>
    ```

    n counts from 1; mean is per channel, 0-255, truncated; nonblack counts pixels whose RGB is not 0. Without the variable, `Present` does no readback.
  - The measured record plan 8 starts from.

- [ ] **Step 1: Write the failing tests.**
  - `frame_log` (`tests/test_d3d9.py`, with `BW2_FRAME_LOG=1`): a 64x64 device cleared to 0xFF102030 and presented twice prints `[bw2] frame 1: 64x64, mean 16,32,48, nonblack 4096 of 4096` and then `frame 2`. Without the variable, no such line appears.
  - `test_boot_to_audio` (`BW2_GAMEFILES=1`, `BW2_FRAME_LOG=1`, fresh `BW2_SUPPORT`). It asserts:
    - exit code 4;
    - exactly one stop line, `reached DirectSound (CoCreateInstance): owned by a later plan`;
    - no line in `LOUD` (as today: `TRIPWIRE`, `trap:`, `unresolved`, `unimplemented`, `uncaught`, `FATAL`, `not implemented`);
    - `scriptlibraryr.dll: lifted DLL at 0x10000000, 0 imports unanswered` in stderr;
    - at least one frame line with nonblack at least half of its pixels.
- [ ] **Step 2: Run them.** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_d3d9.py -k frame_log -v && BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement the frame log.**
- [ ] **Step 4: Bring-up loop.** Run `BW2_HEADLESS=1 BW2_FRAME_LOG=1 BW2_TRACE_IMPORTS=1 out/bw2 --game gamefiles/game`, output in a file, reading its tail.
  - Each loud stop in this plan's layers (sprite, font, the Task 1 answers) or plan 4/5's is fixed with a selftest check first, red then green.
  - A stop in the audio layer is the expected end.
  - A stop at Bink or another later plan's name before audio is recorded, and the boot test's expected stop changes to it.
  - Each stop outside every plan's layer and each tripwire stops the run and goes to the user, as does any disc, volume or driver query from the protection VM.
  - If the game reads input (`GetDeviceData`, `GetDeviceState`, `GetCursorPos`, `GetAsyncKeyState`) before the stop, record where: plan 6 starts from there.
- [ ] **Step 5: Measure and write `docs/research/startup.md`:**
  - the stop line and the time to it (four runs, two with a fresh support folder);
  - the fonts created (faces, heights, count) and the memory font's facts: size, SHA-256, family, and the address that adds it;
  - the sprite and font method calls, with counts;
  - each frame line and the draws in each frame;
  - what the Win32 fixes changed, with the addresses from "Measured";
  - the message pumping and the absence of input reads;
  - the audio start: the calls from `sub_008B1400` and `sub_008B9700` listed in "Measured" item 9, and the audio names in the exe, for plan 8.
- [ ] **Step 6: Update** the spec's section 5.2, `shaders.md` and plan 6's status line as listed under Files. Then run the full suite: `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan7-suite.txt 2>&1; tail -3 .superpowers/sdd/plan7-suite.txt`. Expected: all pass.
- [ ] **Step 7: Audit and commit.** `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through the loading screen to audio"`

## Rulings (my calls; the spec leaves them to the builder)

1. **Scope and phasing:** plan 7 answers what the boot reaches between `D3DXCreateSprite` and audio: the sprite, the font, the memory font and five Win32/COM answers. HLSL compilation (`D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXCreateEffect` from text), `D3DXCreateFragmentLinker` and `D3DXCreateSphere` stay loud later-plan stops. They belong to the first plan whose boot reaches them; none had run on the way to audio. The HLSL library choice (spec 5.2, vkd3d-shader as candidate) is deferred with them.
2. **Font rasterizer:** CoreText and CoreGraphics (Apple system frameworks, no third-party licence, nothing to check against spec 9). Metrics follow GDI's rules from the font's tables, so text layout matches Windows' widths. Glyph shapes are CoreText's.
3. **Memory fonts** stay private to the process and win over a system face of the same name.
4. **A missing face stops loudly** rather than being substituted. Both faces the game asks for (`Arial Narrow`, `Courier New`) ship with macOS 13, and the game supplies its own `Arial Narrow`.
5. **Screen-space sprites** are drawn as pre-transformed vertices moved by -0.5 pixel, and the device's transforms are left alone. `BILLBOARD`, `SetWorldViewRH` and `SetWorldViewLH` stop loudly until the game uses them.
6. **`LoadImageA` of a missing file** answers NULL in BW2's own shim. win32hle's placeholder stays for the other titles it serves.
7. **`LoadLibraryA`'s default `.dll`** and the two ole32 calls are fixed in vendored win32hle, as Windows behaviour any title needs.
8. **The audio boundary** is `CoCreateInstance` of the DirectSound CLSIDs, plus `LoadLibraryA` of a later plan's DLL even when win32hle lists it as a system DLL. Plan 8 decides whether to answer DirectSound or OpenAL.
9. **The frame log** prints numbers only and stores no pixels. Any picture of the game stays on the user's Mac.
