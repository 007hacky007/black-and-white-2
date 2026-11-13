# Plan 10: Bink video, and the boot into the campaign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game plays its Bink movies (the front end's attract video and the campaign's intro), and a scripted click on New Game loads the first land and runs on, past the intro, to the first land's opening scene with no stop.

**Architecture:**
- **Bink through FFmpeg, as the spec says** (5.2 Video). A new `runtime/bw2/video/` layer answers the six `binkw32.dll` imports. Its decoder core is adapted from pcrecomp's MIT `win32hle/bink.c`: libavformat's Bink demuxer reads the movie from the bytes the game hands over (`BINKFROMMEMORY`, a view of `Data/Everything.stuff`), libavcodec decodes it, and `_BinkCopyToBuffer@28` converts the frame into the texture the game has locked. FFmpeg is LGPL-2.1+, linked as separate dynamic libraries (spec 9): only `libavformat`, `libavcodec` and `libavutil`.
- **The land needs five more answers,** each measured with a throwaway stub: two catalog and lifter gaps (a memmove jump table indexed backwards, and seven functions named only by data), `VirtualLock` and `HeapCompact`, a DXT encoder for D3DX (mip chains and loads into DXT1 and DXT5), and effect parameter blocks. A Metal command buffer that runs out of memory during the 23-second land load is fixed in the Metal layer.
- **Measurement:** `BW2_BINK_LOG`, `BW2_QUIT_AFTER_MOVIE` (ends a test at a movie frame, whatever the frame rate) and `BW2_FRAME_DUMP` (PNG frames into a folder outside any git tree, spec 7.3's frame capture) drive the new boot tests.
- **Next layer:** whatever the first land's play asks for next (input on the creature choice, Bink audio tracks, saves). The boot test ends on the land's opening scene.

**Tech Stack:** C and Objective-C (clang, arm64, macOS 13+), the pcrecomp lifter and win32hle (vendored, MIT), FFmpeg 9.0.2 dynamic libraries (Homebrew's, for development; Ruling 2), Metal, ImageIO, Python 3.14 with pefile and capstone, pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 2, 5.2 Video and D3DX, 6.1, 7.2, 7.3, 9, 11). Facts: `docs/research/hlsl.md` ("The boot from the effect to Bink"), `input.md`, `frontend.md`, `build-pipeline.md`, `d3dx-usage.md`.

## Measured before writing (2026-11-13)

Main at `f8b6f5e`, `out/bw2` relinked (143,553,416 bytes), headless, fresh support folder, `BW2_INPUT_SCRIPT` = `tests/test_boot.py`'s `PROFILE_SCRIPT`: exit 4,

```
[bw2] reached _BinkOpen@8: owned by a later plan (from guest 0xDEAD0000, sub_0093C7B0)
```

A throwaway spike branch (deleted; nothing kept; main's engine rebuilt after it) answered each stop below with a logging stub to see the next one. Frames were looked at as PNGs in a scratch folder outside the repo and deleted.

1. **The movie's bytes.** Just before the call the game's CRT `CreateFileA` asks for `data/splash/attract.bik`, which no install holds (both manifests list one `.bik`, `Data/Splash/EAlogocompressed.bik`: 644,220 bytes, 720x540, 120 frames at 30/1 fps, one audio track, `binkaudio_dct` 44,100 Hz stereo). It then maps a view of `Data/Everything.stuff` (`sub_009240C0`) and calls `_BinkOpen@8(ptr, 0x04000000)`: `BINKFROMMEMORY`, the pointer at the movie inside the view. Answering 0 crashes: `FATAL: signal 10 ... guest 0x00000014 in guest sub_0093C7B0`. The game reads the handle at `+0x14` without a NULL check.
2. **The movies.** `Data/Everything.stuff` (1,427,446,404 bytes) holds 122 Bink 1 movies, all revision `i` with video flags 0 (no alpha): 94 at 400x300, 23 at 512x384, 3 at 720x576, 1 at 640x360, 1 at 320x240. 22 have one audio track, 100 have none. The attract is at offset `0x30D3C440` (32,540,316 bytes, 720x576, 2,033 frames at 25/1 fps, no audio). The intro is at `0x3B02BD00` (51,782,036 bytes, 640x360, 1,798 frames at 5000000/208333 fps, no audio). Homebrew's FFmpeg 9.0.2 decodes both with no error (1.2 s and 1.6 s), and also a 400x300 one with a track (`binkaudio_dct`, 44,100 Hz stereo). Both come out as `yuv420p`. The attract's luma reaches 0 and 255 (1,183 frames have a minimum below 16), the intro's spans 10 to 241.
3. **The call pattern** (stubs that log every call). After the open, `D3DXCreateTexture` (thunk `sub_00B522F4`) and `GetLevelDesc` (`sub_0093C5D0`). Then once per rendered frame `sub_0093C460` calls `_BinkGoto@12(h, n, 0)`, `_BinkDoFrame@4(h)`, `IDirect3DTexture9::GetSurfaceLevel`, `_BinkDX9SurfaceType@4(surface)`, `IDirect3DSurface9::LockRect`, `_BinkCopyToBuffer@28(h, bits, pitch, height, 0, 0, 3)`, `UnlockRect` and `Release`. The pitch and height are 2880 and 576 for the attract, 2560 and 360 for the intro. `n` follows the movie's clock: the same `n` is asked many times at the headless frame rate, it jumps forward when frames are slow, and it never goes back. `_BinkNextFrame@4` and `_BinkWait@4` are not imported. The attract reached frame 2,020 of 2,033 and was closed 111 s after its open (30,703 Goto/DoFrame/Copy rounds). The game reopened it at once from a new view. `GetSurfaceLevel` hands back a new surface object every frame, because ours do not outlive the game's `Release` (a D3D9 difference, noted for a later plan).
4. **binkw32.dll 1.8.0.0** (game tree, SHA-256 `b0a26df589edc62a660fc53649ad0f2ecb5f01cff9ec5ba8b21c917627f5e826`) exports 88 names; `white.exe` imports 6: `_BinkGoto@12`, `_BinkClose@4`, `_BinkCopyToBuffer@28`, `_BinkDoFrame@4`, `_BinkDX9SurfaceType@4`, `_BinkOpen@8` (IAT `0xC12558`-`0xC1256C`). Its `_BinkDX9SurfaceType@4` (`0x1800EB50`) calls the surface's `GetDesc` (vtable `+0x30`) and maps the format: 20 (R8G8B8) to 1, 21 (A8R8G8B8) to 5, 22 (X8R8G8B8) to 3, 23 (R5G6B5) to 10, 24 (X1R5G5B5) to 9, 25 (A1R5G5B5) to 8, 26 (A4R4G4B4) to 7, 30 (X4R4G4B4) to 7, `YUY2` to 13, `UYVY` to 14, and NULL or any other format to `0xFFFFFFFF`. The DLL imports no DirectSound; it loads sound DLLs with `LoadLibraryA`.
5. **New Game.** `PROFILE_SCRIPT` plus `300 move -170 284`, `310 down 0`, `314 up 0` (the running point goes from (422, 427) to (252, 711), the front end's New Game button at 1024x768) clicks New Game before the attract's timer. The game then loads the first land. The stops came in this order:
   - a. `[win32hle] FATAL: unimplemented import VirtualLock (0x7F0033E0) called from guest 0x009B5090`. Calls: `(0x35670010, 2097152)` and `(0x39C52040, 2101250)` from `sub_009B5090`, both again from `sub_00A02A40`. No `VirtualUnlock`.
   - b. `[bw2] FATAL: d3d9: D3DX: making DXT mip levels needs a compressor (not supported)` in `D3DXCreateVolumeTextureFromFileInMemoryEx` (thunk `sub_00B5230C`): a DXT1 128x128 volume whose levels 1-7 are generated.
   - c. `[bw2] FATAL: d3d9: ID3DXEffect::BeginParameterBlock not implemented (from guest 0xDEAD0000, sub_00963C80)`. At least 10 blocks are recorded there, and `ApplyParameterBlock` follows.
   - d. `[bw2] FATAL: d3d9: D3DX: compressing into DXT is not supported` in `D3DXLoadSurfaceFromSurface` (thunk `sub_00B52360`): A8R8G8B8 512x512 into DXT5, several times. Each is followed by `D3DXFilterTexture` (thunk `sub_00B5235A`) building levels 1-9 of that DXT5 512x512 texture.
   - e. `[bw2] FATAL: unresolved indirect call at 0x00AD468C (in sub_00AD43D0)`. That is the CRT memmove's backward tail: `neg ecx` then `jmp [ecx*4 + 0xAD4668]` at `0xAD4596`, whose slots lie before the displacement (`0xAD468C` is slot `0xAD465C`, index -3). disasm32 reads tables forward only. The byte pattern (`neg r32; jmp [r32*4 + disp32]`) occurs at `0xAD4596` (table end `0xAD4668`) and `0xAD88C6` (`0xAD8998`) in `white.exe`, at `0x1003B226` (`0x1003B2F8`) and `0x1003EE66` (`0x1003EF38`) in `ScriptLibraryR.dll`, and nowhere in `d3dx9_25.dll`. With the backward slots read (a full relift, 319 s), the boot goes on.
   - f. `HeapCompact(1, 0)` from `sub_006139E0`, right after the land's init. The game's debug output puts that init at 23.2 to 23.6 s, 22.7 to 22.9 s of it in its map script.
   - g. `[bw2] FATAL: unresolved indirect call at 0x00746700 (in sub_10007E20)`, from `ScriptLibraryR.dll` into `white.exe`. `0x746700` follows int3 padding (`0x7466FB`-`0x7466FF`), and only a data table names it: the script native table, 152 entries of `0x90` bytes from `0x171E540` to `0x1723A30`, where it is the one entry missing from the catalog. The catalog's `sub_007466A7` claims an extent to `0x746753` across the padding, so `immediate_starts`' "not covered" rule hides it. A scan for 4-byte-aligned dwords in `.rdata`/`.data` that name a 16-aligned `.text` address right after int3 padding, not itself int3 and not a start, finds exactly 7 in `white.exe`: `0x42FFD0`, `0x724310`, `0x724870`, `0x72B5B0`, `0x746700`, `0x746C00`, `0x7A49E0`. It finds none in `ScriptLibraryR.dll` or the rebased `d3dx9_25.dll`.
   - h. With all of these answered (and the movie stubs), the game opens the intro (frame about 606), and after it draws the first land's opening scene in 3D. It ran to the 300 s timeout (16,729 frames; the last 67% non-black) with no further stop, no loud line and no tripwire.
6. **Metal.** Once per run, during the land load (no `Present` for the whole map load), one command buffer fails with `[bw2] metal: command buffer failed: Error Domain=MTLCommandBufferErrorDomain Code=8 "Insufficient Memory (00000008:kIOGPUCommandBufferCallbackErrorOutOfMemory)"`. The next frame line is 496. The Mac has 32 GB. Later frames show black areas on the land. The spike's DXT stubs also left textures black, so the cause of each black area is not separated yet.
7. **A repeated folder.** One file name the game asks for repeats the folder `data\art\water\` nine times before its file part (a missing bump-map texture). It is not yet known whether Windows builds the same name.
8. **Absent** in every run: any disc, volume or driver question, any tripwire, any import from the protection VM. No `_BinkOpen@8` comes before the front end: the EA logo movie is never opened.
9. **FFmpeg.** Homebrew's `ffmpeg` 9.0.2 is installed (`/opt/homebrew/opt/ffmpeg`; configured `--enable-gpl --enable-version3`, so a GPL build: fine on this Mac, never shipped). The release tarball `ffmpeg-9.0.2.tar.xz` (2026-09-18, 12,040,788 bytes) has SHA-256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`. Building it from source was not tried here (Ruling 2).

**The engine today:**
- `runtime/bw2/later.c` lists `binkw32.dll` in `g_later_dlls`, so the six imports stop by name.
- `runtime/bw2/absent.c` defines an empty `win32hle_bink`, and `bw2/build.py`'s `WIN32HLE_CORE` leaves out `bink.c` (it needs FFmpeg and win32hle's SDL audio).
- `runtime/bw2/d3dx/d3dx_tex.c` stops on DXT in three places: `convert` (line 136), `make_mip` (184) and the fill path (443).
- `runtime/bw2/d3dx/fx_effect.c` has no parameter blocks (`d3d9_methods.c` names the three methods, `unimplemented` stops).
- `third_party/pcrecomp/tools/disasm/disasm32.py` `table_entries` reads slots forward from the displacement.
- `bw2/extrastarts.py` has `immediate_starts` (code immediates, not covered by a catalogued extent), `vm_pattern_starts`, and the `"boot"` reason.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings, bytes, movies or frames from them, decoded pictures, shaders or effects from the game, or generated C. Facts only (addresses, sizes, counts, hashes, offsets). No test or switch writes a movie, a decoded frame or the game's text into the repo. `BW2_FRAME_DUMP` refuses any folder inside a git work tree. Run `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` before every commit that adds data; it must say CLEAN.
- Docs (this plan, `campaign.md`, the spec) never quote the game's own output or strings: a debug line or a script name is described in neutral words with the address that prints or holds it. File, DLL and API names are facts.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The tripwire stays on and its allow list (`runtime/bw2/guard.c` `g_allowed`) stays empty. Winsock keeps answering "no network". A disc, volume or driver question from any code during the land load stops the work and goes to the user with the evidence.
- Fail loudly, never return 0 silently:
  - every Bink argument or flag this plan has not measured stops with `[bw2] FATAL: bink: <what>` and the guest caller. A movie the decoder cannot open stops the same way, naming the reason (the game does not check for NULL, "Measured" item 1);
  - a movie with an audio track exits 4 with `[bw2] reached Bink audio track: owned by a later plan` (Ruling 1);
  - parameter-block misuse the game has not shown (nested begin, end or apply with no block, a foreign handle) stops naming the method;
  - a DXT format the encoder does not write (DXT2, DXT4) stops naming the format.
- **Later plans' layers** each exit 4 with `[bw2] reached <name>: owned by a later plan`. `binkw32.dll` leaves `g_later_dlls`. `dinput.dll`, `openal32.dll` and `wrap_oal.dll` stay, as do `DirectDrawCreate` and `DirectDrawEnumerateExA`.
- **`bw2.h` and the win32hle headers do not change** (every lifted shard includes them; a change rebuilds 55,000 functions). New cross-file functions are declared `extern` where used, as `register.c` does, or in a header under a `BW2_SUBDIRS` folder (a 2-minute host rebuild).
- Exit codes are unchanged: 0 normal (including `BW2_QUIT_AFTER_READS` and `BW2_QUIT_AFTER_MOVIE`), 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- Headless (`BW2_HEADLESS=1`) for every test. Tests that need game files carry `@pytest.mark.gamefiles` (`BW2_GAMEFILES=1`).
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES` (path relative to `third_party/pcrecomp`, what changed and why). No NFSU2Recomp code. No FFmpeg, Wine or ReactOS source is copied into the repo. pcrecomp's MIT `bink.c` may be adapted, keeping a line naming where it came from.
- Third-party code: FFmpeg only as separate dynamic libraries (`libavformat`, `libavcodec`, `libavutil`), found under `BW2_FFMPEG_PREFIX` or `/opt/homebrew/opt/ffmpeg`. No new Python package. Homebrew's `ffmpeg` is already installed; no other Homebrew package is added.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Never push, never add a remote, never change git config, never write the user's real name or email anywhere. Work on branch `plan10`.

## Review Focus

- **A movie held open past its end.** The intro is never closed after its last frame, and the attract runs to frame 2,020 of 2,033. Asking the last frame again and again must cost nothing and must keep showing that frame, with no read past the movie's bytes. (Task 5, check `bink_last_frame`.)
- **A locked texture smaller than the movie, or offset.** `_BinkCopyToBuffer@28` must clip to the destination height and pitch it is given, and must never write a byte outside `height * pitch`. (Task 5, check `bink_copy_clip`: guard bytes around a half-height buffer and a 2-pixel x offset stay untouched.)
- **Two movies at once, and reopening.** The attract is closed and reopened from a new view, and a later scene could open a second movie before closing the first. Each handle must decode on its own state. (Task 5, check `bink_two_movies`; Task 6, check `binkw32_reopen`.)
- **DXT chains below 4x4.** A 512x512 DXT5 chain has 2x2 and 1x1 levels, and the 128x128 DXT1 volume has 1x1x1 levels. The encoder must pad a partial block from the level's own pixels and give the box average there, never read past the level. (Task 3, check `dxt_small_levels`.)
- **The GPU during a long load with no Present.** Uploads that pile up for 23 s must never fail a command buffer or lose a texture. (Task 7, check `mtl_upload_flood`, written after Task 7 Step 1 measures the real cause, with a guest footprint under 256 MB.)
- **Goto outside the movie.** The game's `sub_0093C460` asks `n = int(fps * t) + 1` with no upper clamp, so once the intro's time passes its length it asks `Frames + 1` and more. `_BinkGoto@12` must do what binkw32 1.8's own code at `0x18011A60` does: frame 0 becomes 1, a frame above `Frames` becomes `Frames`. Only nonzero flags and unknown handles stop. (Task 6, check `binkw32_goto_clamp`; Ruling 5.)
- **Parameter-block semantics.** While a block records, a setter writes only into the block: the parameter's live value and its dirty mark do not change until `ApplyParameterBlock`. Applying at once would leave the last of the game's 10 or more recorded blocks live and rendered, which no other test would see. (Task 4, check `fx_block_record`; Ruling 10.)
- **The GPL FFmpeg stays on this Mac.** Development links Homebrew's GPL-enabled FFmpeg, so `out/bw2` must never be copied off the machine or bundled. A `brew upgrade ffmpeg` that bumps a library's major version breaks `out/bw2` until it is relinked. (Task 5, check `test_engine_ffmpeg_links`, which records the linked install names and versions; Ruling 2.)

---

### Task 1: The memmove's backward tables and the data-named starts

**Files:**
- Modify: `third_party/pcrecomp/tools/disasm/disasm32.py` (`table_entries_before`, `negated_index`, `jump_table_targets`, and every reader of jump tables: the function walk at line 557, `body_instructions`' interior walk at line 1287 and `close_dispatch_targets` at line 1417), `third_party/pcrecomp.PATCHES`.
- Modify: `bw2/discover.py` (`_decoded_marks` and `_table_span`: the backward slots and their arms).
- Modify: `bw2/extrastarts.py` (`data_starts`, the `"data"` reason, its `--write` path), `bw2/catalog/white-1.42.extra-starts.json` (the 7 starts).
- Test: `tests/test_lifter_patches.py`, `tests/test_extrastarts.py`, `tests/test_discover.py`.

**Interfaces:**
- Produces:
  - `table_entries_before(read, table: int, is_code, limit: int = 8) -> list[int]`: the code addresses in the slots at `table - 4`, `table - 8`, and so on, up to `limit` slots, stopping at the first slot that is not a code address.
  - `negated_index(prev, jmp) -> bool`: True when `jmp` is `jmp [r*4 + disp32]` and `prev`, the instruction just before it on the same path, is `neg r` of that same register. One helper, so every reader decides the same way.
  - `Disassembler.jump_table_targets(insn, limit: int = 256, negated: bool = False) -> list`: with `negated`, the arms are slot 0 (if it is code) plus `table_entries_before(...)`.
  - Every caller gets the rule, so discovery, `reloc_code_targets` and the lifter agree on the backward arms at `0xAD466C`-`0xAD469C`, which only those slots name:
    - the function walk (line 557) passes `negated=negated_index(prev, insn)`;
    - `body_instructions` (line 1287) and `close_dispatch_targets` (line 1417) call `table_entries` directly. Each keeps the instruction before the `jmp` and, when `negated_index` holds, also takes `table_entries_before`. They must reach the same arms as the function walk, or an interior walk leaves a backward arm out of the extent;
    - `bw2/discover.py`'s `_decoded_marks` keeps the previous instruction, passes `negated` to `jump_table_targets`, and marks the backward slot span as covered (a new `_table_span(dis, table, negated=True)` that returns `(table - 4 * k, table + 4)` for the `k` code slots before `table`, plus slot 0). Otherwise `reloc_code_targets` sees those slots as relocated code pointers outside every decoded instruction and table, and may seed their arms as function starts with extents of their own.
  - `bw2.extrastarts.data_starts(exe: Path, catalog: Path) -> list[int]`: sorted addresses named by a 4-byte-aligned dword in `.rdata` or `.data` (only those two sections, only aligned offsets) that are 16-byte aligned inside `.text`, follow an `0xCC` byte, do not start with `0xCC`, and are not catalogued starts. A catalogued extent that covers them does not hide them ("Measured" item 5g). Rechecked on 2026-11-13 under exactly this rule: 7 in `white.exe`, none in `ScriptLibraryR.dll`, none in the rebased `d3dx9_25.dll`. A scan over every non-executable section at every byte offset finds 3 more in `white.exe`, and none of them is a start: `0x504280` (an unaligned `.data` hit) and `0x580090` (named in `_PDATA`) both begin inside an instruction whose `disp32` holds the `0xCC` byte. `0xA300A0` (named in `_PDATA`) looks like a body after padding, but the `_PDATA` dwords around both hits are packed 16-bit pairs (`0x00D00113`, `0x00010033`), not a pointer table, and nothing that holds addresses names it. If anything ever reaches it, the lift stops loudly with its address. MSVC keeps function pointers (vtables, native tables, callbacks) aligned in `.rdata` and `.data`.

- [ ] **Step 1: Write the failing tests.**
  - `test_negated_index_table_arms` (`tests/test_lifter_patches.py`): a synthetic PE (`fixtures.make_pe`). Its function at `0x401000` does `neg ecx; jmp dword ptr [ecx*4 + 0x401060]`. Three dwords at `0x401054`, `0x401058` and `0x40105C` point at arms `0x401020`, `0x401030` and `0x401040`. Each arm is `mov eax, imm32; ret`. Then: `jump_table_targets(insn, negated=True)` returns all three; with `negated=False` it returns none of them; `negated_index` is True for the `neg ecx` pair and False when the `neg` names another register; `body_instructions` and `close_dispatch_targets` reach all three arms; and the lifted C for the function has a label for each arm (no `RECOMP_ITAIL` dispatch to them).
  - `test_negated_table_marks` (`tests/test_discover.py`): on the same synthetic PE, `_decoded_marks` marks `0x401054`-`0x401060` as covered and each arm as an instruction start. On `white.exe` (gamefiles), `reloc_code_targets` returns no address in `0xAD466C`-`0xAD469C`, and `_decoded_marks` covers every slot from `0xAD465C` up to and including slot 0 at `0xAD4668`.
  - `test_data_named_start_inside_extent` (`tests/test_extrastarts.py`): a synthetic PE whose catalogued function claims an extent across int3 padding to a 16-aligned start that only a `.data` dword names. `data_starts` returns it; `immediate_starts` does not. The same address named only by an unaligned `.data` dword, or only by a dword in a third non-executable section, is not returned.
  - `test_white_data_starts` (gamefiles): `data_starts` on `white.exe` with the committed catalog returns exactly `[0x42FFD0, 0x724310, 0x724870, 0x72B5B0, 0x746700, 0x746C00, 0x7A49E0]`, and on `ScriptLibraryR.dll` and the rebased `d3dx9_25.dll` with their catalogs returns `[]`. Each of them disassembles to at least one whole instruction before any `0xCC`. `load_extra_starts` on the committed file holds all 7 with reason `"data"`.
  - `test_white_memmove_arms` (gamefiles): disassembling `white.exe` function `0xAD43D0` gives `jump_targets` that include `0xAD468C` and the other code slots before `0xAD4668`. The catalogued function holding `0xAD88C6` (table end `0xAD8998`) gets the same treatment. `ScriptLibraryR.dll`'s two sites are checked the same way.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_lifter_patches.py tests/test_extrastarts.py -v` (and with `BW2_GAMEFILES=1`). Expected: FAIL on the new tests.
- [ ] **Step 3: Implement the disasm32 change** (a vendored change: its own commit and one `PATCHES` line: "tools/disasm/disasm32.py - a `jmp [r*4+T]` right after `neg r` also takes the slots before T (MSVC memcpy/memmove CopyUnwindDown), in the function walk, body_instructions and close_dispatch_targets - BW2's CRT memmove reached 0xAD468C unlabelled"). The `bw2/discover.py` change is ours, in the second commit. Run `.venv/bin/python third_party/pcrecomp/tools/lift/difftest.py` before and after it; the pass count must not drop (404/405, `sse.sqrtps` failing before and after).
- [ ] **Step 4: Implement the `_decoded_marks` change and `data_starts`,** add the 7 starts to `white-1.42.extra-starts.json` with reason `"data"`, and make `python -m bw2.extrastarts --write` keep them.
- [ ] **Step 5: Relift and check.** `.venv/bin/python -m bw2.pipeline > <scratch>/pipeline.txt 2>&1` (about 5 minutes). Expected: exit 0. The `[link]` line's unresolved-target count is not higher than before (9 at `f8b6f5e`). Then `.venv/bin/pytest tests/test_lift.py tests/test_discover.py tests/test_extrastarts.py tests/test_lifter_patches.py -q`: all pass. A pinned count that changes because of the new arms or starts is updated, with the reason in the test's comment.
- [ ] **Step 6: Commit** (two commits: the vendored disasm32 change first). `git commit -m "Read jump tables indexed by a negated register"`, then `git commit -m "Add backward table arms and data-named starts"`

### Task 2: VirtualLock, VirtualUnlock and HeapCompact

**Files:**
- Modify: `runtime/bw2/shims_k32.c`, `third_party/pcrecomp/runtime/win32hle/guest_mem.c` (`hle_vm_largest_free`, `hle_vm_is_reserved`), `third_party/pcrecomp.PATCHES`.
- Test: `runtime/bw2/shims_selftest.c`, `tests/test_engine.py::test_shims_selftest`.

**Interfaces:**
- Produces:
  - `uint32_t hle_vm_largest_free(void)` in `guest_mem.c`: the size of the largest range `hle_vm_reserve(0, n)` could hand out now (the same first-fit walk as `find_gap`, under `g_mem_lock`). It is declared `extern` in `shims_k32.c`, not in `win32hle.h`.
  - `int hle_vm_is_reserved(uint32_t va, uint32_t size)` in `guest_mem.c`: 1 when every byte of `[va, va + size)` lies in ranges of `g_rng` (the static reservation table), else 0; `size` 0 or a range that wraps gives 0. `g_rng` holds every guest reservation: `VirtualAlloc` (`kernel32.c`), the heap's carves and large blocks (`small_alloc` and `hle_heap_alloc` reserve through `vm_reserve_locked`), stacks and loaded images. So one helper answers "mapped" for heap and `VirtualAlloc` memory alike, which `k_VirtualQuery` (it knows only `VirtualAlloc` regions) cannot. Declared `extern` in `shims_k32.c`. Its own `PATCHES` line, in the same vendored commit as `hle_vm_largest_free`.
  - `VirtualLock(addr, size)`: TRUE when `hle_vm_is_reserved(addr & ~0xFFF, page-rounded size)` holds, and the range is remembered. A decommitted page inside a reservation still counts: win32hle maps reservations read-write for their whole life, so the page is there to lock. Otherwise FALSE with `ERROR_INVALID_ADDRESS` (487). Nothing is pinned on the host: guest memory is never paged out under the game.
  - `VirtualUnlock(addr, size)`: TRUE for a range a `VirtualLock` covered, which is then forgotten. Otherwise FALSE with `ERROR_NOT_LOCKED` (158).
  - `HeapCompact(heap, flags)`: `min(hle_vm_largest_free(), 0x60000000)`, so it never disagrees with `GlobalMemoryStatus`'s 1.5 GB available. win32hle has one heap, handle 1, which `GetProcessHeap` and `HeapCreate` both return. Any other handle gives 0 with `ERROR_INVALID_HANDLE` (6).
  - The locked ranges live in a growable array (spec 7.2: no fixed-size tables).

- [ ] **Step 1: Write the failing checks** in `shims_selftest.c`:
  - `VirtualLock` on a 2,097,152-byte `HeapAlloc` block, and on a 2,101,250-byte block at an address ending in `0x40` ("Measured" item 5a), gives 1. `VirtualUnlock` of each gives 1, and a second `VirtualUnlock` gives 0 with `GetLastError() == 158`;
  - `VirtualLock` on a 65,536-byte `VirtualAlloc(0x7E000000, ..., MEM_RESERVE | MEM_COMMIT)` region (a fixed address far above first-fit's reach) gives 1, and on 8,192 bytes from `0x7E00F000`, which run one page past that region's end, gives 0 with 487;
  - `VirtualLock(0x00001000, 4096)` (unmapped) gives 0 with 487;
  - `HeapCompact(GetProcessHeap(), 0)` gives a value in `(0, 0x60000000]` that is a multiple of 4096. `HeapCompact(0xDEADBEEF, 0)` gives 0 with 6.
- [ ] **Step 2: Run it.** `.venv/bin/pytest tests/test_engine.py -k shims_selftest -v`. Expected: FAIL (the three imports are not answered).
- [ ] **Step 3: Implement `hle_vm_largest_free` and `hle_vm_is_reserved`** in `guest_mem.c` from its range list, as one vendored commit with a `PATCHES` line for each. Before writing the `HeapCompact` answer, read `sub_006139E0` and record what the game does with the value (Task 8's research doc).
- [ ] **Step 4: Implement the three shims** in `shims_k32.c` and register them in its table.
- [ ] **Step 5: Run it.** Expected: PASS. Then `.venv/bin/python -m bw2.pipeline --build-only`.
- [ ] **Step 6: Commit.** `git commit -m "Add guest range queries to win32hle"`, then `git commit -m "Answer VirtualLock, VirtualUnlock and HeapCompact"`

### Task 3: DXT compression in D3DX

**Files:**
- Create: `runtime/bw2/d3dx/d3dx_dxt.c`.
- Modify: `runtime/bw2/d3dx/d3dx.h` (declaration), `runtime/bw2/d3dx/d3dx_pixel.c` (`d3dx_write` writes DXT), `runtime/bw2/d3dx/d3dx_tex.c` (the three `dx_fail` sites).
- Test: `runtime/bw2/d3dx/d3dx_selftest.c`, `tests/test_d3dx.py`.

**Interfaces:**
- Produces:
  - `void d3dx_dxt_encode(uint32_t fmt, const float rgba[64], uint8_t *block)`: one 4x4 block, 16 texels of RGBA in [0, 1] in row order, into 8 bytes (`D3DFMT_DXT1`) or 16 bytes (`D3DFMT_DXT3`, `D3DFMT_DXT5`). `D3DFMT_DXT2` and `D3DFMT_DXT4` stop loudly.
  - `d3dx_write(fmt, in, w, h, dst, pitch)` with a DXT `fmt` encodes `ceil(w/4) * ceil(h/4)` blocks. `pitch` is the block-row pitch. A partial edge block is padded by repeating the level's last column and row.
  - In `d3dx_tex.c`: `convert` into DXT decodes the source to float and encodes (scaling and filtering as for any other format). A destination rect that is not 4-aligned decodes the touched blocks, merges, and encodes them again. `make_mip` and the fill path decode, filter and encode.
- The encoder (Ruling 9): colour along the principal axis of the block's colours (power iteration on their covariance), endpoints from the projections' extremes rounded to 5:6:5, then one least-squares refit of the endpoints from the chosen indices. DXT1 uses its 3-colour mode with index 3 transparent when any texel has alpha < 0.5, and only the opaque texels shape the colour fit. DXT3 alpha is `round(a * 15)`. DXT5 alpha uses the 8-value mode between the block's minimum and maximum alpha (nearest index). Equal endpoints give all-zero indices.

- [ ] **Step 1: Write the failing checks** (`d3dx_selftest.c`, one group each, each test in `test_d3dx.py` expecting `<group>: 0 failed`):
  - `dxt_exact`: a solid block whose colour is exact in 5:6:5 decodes (with plan 4's `d3dx_read`) to exactly that colour, for DXT1, DXT3 and DXT5. A block of two such colours decodes exactly. A DXT5 block with alpha 0 and 1 only decodes alpha exactly;
  - `dxt_punch`: a DXT1 block with 5 texels at alpha 0.2 and the rest opaque decodes those 5 with alpha 0 and the rest with alpha 1, in 3-colour mode (`color0 <= color1`);
  - `dxt_quality`: a 64x64 smooth RGB gradient survives a DXT1 round trip with RMS error at most 6/255 per channel, and an alpha ramp survives DXT5 with alpha RMS at most 3/255;
  - `dxt_load_surface`: `D3DXLoadSurfaceFromSurface` from a 512x512 A8R8G8B8 surface into a DXT5 surface gives `D3D_OK`, and the decoded result is within the `dxt_quality` bounds of the source. A 6x6 source into a 4-aligned rect works too, and into a rect at (2,2) only the touched blocks change;
  - `dxt_filter`: `D3DXFilterTexture` on a 512x512 DXT5 texture with 10 levels fills levels 1 to 9. Each decoded level is within 8/255 RMS of the box-filtered decode of the level above;
  - `dxt_small_levels`: the 2x2 and 1x1 levels of that chain decode to the box average of the level above within 8/255, and a 128x128 DXT1 volume texture made by `D3DXCreateVolumeTextureFromFileInMemoryEx` from a one-level in-memory DDS that the test builds (depth 4, mip count 0) gets 8 levels whose 1x1x1 level is the average of the top within 8/255;
  - `dxt_unsupported`: in a child, a DXT2 destination stops with a line naming `DXT2`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_d3dx.py -k dxt -v`. Expected: FAIL.
- [ ] **Step 3: Implement `d3dx_dxt.c`, then the `d3dx_pixel.c` and `d3dx_tex.c` changes.** The three `dx_fail` lines go.
- [ ] **Step 4: Run them, then the D3DX and effect suites.** `.venv/bin/pytest tests/test_d3dx.py tests/test_effects.py tests/test_ui.py -q`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Compress DXT1, DXT3 and DXT5 in D3DX"`

### Task 4: Effect parameter blocks

**Files:**
- Modify: `runtime/bw2/d3dx/fx_effect.c`, `runtime/bw2/d3dx/fx.h` (the effect's block list and recording state).
- Test: `runtime/bw2/d3dx/fx_selftest.c`, `tests/test_effects.py`.

**Interfaces:**
- Produces (Ruling 10; the behaviour of native d3dx9 as Wine's `dlls/d3dx9_36/effect.c` implements it and its `tests/effect.c` parameter-block test, written against native, checks it):
  - `ID3DXEffect::BeginParameterBlock(this)`: `D3D_OK`, and recording starts;
  - while recording, every parameter setter (`SetValue`, `SetBool`/`Int`/`Float` and their arrays, `SetVector*`, `SetMatrix*` in all forms, `SetString`, `SetTexture`) writes **only into the block**. The parameter's live bytes and its dirty serial do not change, and a `Get*` still reads the value from before `Begin` (Wine: `param_get_data_and_dirtify` hands the setter `record_parameter`'s buffer instead of the parameter's data, and skips `set_dirty`, while `current_parameter_block` is set). Each set appends one record (the parameter, its bytes after the set) in call order, as Wine's `record_parameter` does. The record starts as a copy of that parameter's newest record in this block, else of its live bytes, so a partial set (an array setter given fewer elements) composes with what came before. Then the setter writes into it through its usual code;
  - references: a record holds one reference to each texture (and other object) it names. The setter's own reference swap runs on the record, not on the live slot, so the live slot and its reference are untouched until `Apply` (Wine: `set_value` does the `AddRef`/`Release` on the destination it is handed, and `free_parameter_block` releases the records' objects). A recorded texture's count is therefore one higher per record naming it until the effect is released;
  - the hook is one function that every setter already passes through, at the point where it now writes `p->data` and calls `dirty(p)`: `uint8_t *fx_dest(struct fx_effect *e, struct fx_param *p)` returns `p->data` normally and the new record's bytes while recording, and `dirty(p)` is skipped while recording;
  - `ID3DXEffect::EndParameterBlock(this)`: a nonzero `D3DXHANDLE`, unique within the effect, and recording stops;
  - `ID3DXEffect::ApplyParameterBlock(this, h)`: `D3D_OK`; each record in order is written into the live parameter through the `SetValue` path (`swap_refs` on the live slot, so the live slot gains its own reference, then the copy and a dirty mark). The block keeps its records and references and can be applied again;
  - blocks live until the effect is released (d3dx9_25 has no `DeleteParameterBlock`); `CloneEffect` does not copy them;
  - stops naming the method: Begin while recording, End or Apply with no block, Apply of a handle this effect did not make.

- [ ] **Step 1: Write the failing checks** (`fx_selftest.c` groups, with `test_effects.py` tests), on an effect the test builds with its own fx_2_0 writer (`tests/fxwrite.py`) or one already used by the fx groups:
  - `fx_block_record`: `SetFloat(a, 1)`; Begin; `SetFloat(a, 2)`; `SetFloat(a, 3)`; `SetTexture(t, tex)`; `GetFloat(a)` is still 1 and `GetTexture(t)` is still NULL while recording. End gives `h != 0`, and `GetFloat(a)` is still 1 and `GetTexture(t)` still NULL after it. The parameter's dirty serial is the same before Begin and after End. `SetFloat(a, 5)`, then `Apply(h)` gives `GetFloat(a) == 3`, `GetTexture(t) == tex`, and a new dirty serial for both. A second `SetFloat(a, 5)` and `Apply(h)` give 3 again;
  - `fx_block_partial`: a 4-element float array `v` set to (1, 2, 3, 4); Begin; `SetFloatArray(v, {9}, 1)`; End; Apply gives (9, 2, 3, 4);
  - `fx_block_refs`: recording `SetTexture(t, tex)` raises the texture's reference count by one (the record's) and leaves the live slot NULL; `Apply` raises it by one more (the live slot's); the effect's last `Release` brings it back to the start;
  - `fx_block_two`: two blocks on one effect apply their own values, and a block's handle differs from every parameter handle;
  - `fx_block_misuse`: in children, Begin twice, End with no Begin, and Apply of `0x7000` each stop with a line naming the method.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_effects.py -k block -v`. Expected: FAIL.
- [ ] **Step 3: Implement** `fx_dest` and the skipped `dirty(p)` in every setter, the block list in `struct fx_effect`, and the three methods in `g_impls`. Wine's code is read for behaviour only; nothing from it is copied.
- [ ] **Step 4: Run them, then `tests/test_effects.py tests/test_hlsl.py -q`.** Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Record and apply effect parameter blocks"`

### Task 5: FFmpeg in the build, and the Bink decoder core

**Files:**
- Create: `runtime/bw2/video/bink_dec.c`, `runtime/bw2/video/bink_dec.h`, `runtime/bw2/video/video_selftest.c`, `tests/test_video.py`.
- Modify: `bw2/build.py` (`ffmpeg_prefix`, `BW2_SUBDIRS` gains `"video"`, `engine_libs`, `HOST_LIBS` made lazy, a per-source include for `runtime/bw2/video/`), `tests/test_build.py`.

**Interfaces:**
- Produces:
  - `bw2.build.ffmpeg_prefix() -> Path`: `$BW2_FFMPEG_PREFIX` if set, else `/opt/homebrew/opt/ffmpeg`. It must hold `include/libavformat/avformat.h`, `include/libavcodec/avcodec.h`, `include/libavutil/avutil.h` and `lib/libavformat.dylib`, `lib/libavcodec.dylib`, `lib/libavutil.dylib`. Otherwise it raises `ToolsMissing("FFmpeg not found under <prefix>: brew install ffmpeg, or set BW2_FFMPEG_PREFIX")`. It reads the environment each time it is called.
  - Only the engine host needs FFmpeg. `HOST_LIBS` today is a dict built at import time (`{"linkcheck": [], "engine": engine_libs()}`), so an `engine_libs()` that calls `ffmpeg_prefix()` would make `import bw2.build` fail on any machine without FFmpeg (the linkcheck host and every test that imports `bw2.build` included), and a `BW2_FFMPEG_PREFIX` set after import would be ignored. It becomes lazy: `HOST_LIBS = {"linkcheck": lambda: [], "engine": engine_libs}`, called inside `build()` as `HOST_LIBS[host]()`, after `check_tools()`. `engine_libs()` adds `-L<prefix>/lib -lavformat -lavcodec -lavutil`.
  - `-isystem <prefix>/include` goes only on the compile jobs of sources under `runtime/bw2/video/` (in `build()`'s host job list), not into `bw2_includes()` or the shard flags. `bink_dec.h` includes no FFmpeg header (its types are its own), so no other file needs FFmpeg's headers. Adding `video` to `BW2_SUBDIRS` and its header to the digest still rebuilds every shard once (about 80 s); the FFmpeg prefix itself never enters the shards' stamp. It does enter the engine host's stamp, so a changed prefix recompiles the host objects (seconds), not the shards.
  - `runtime/bw2/video/bink_dec.h`:
    ```c
    struct bk_movie;
    struct bk_info { uint32_t width, height, frames, rate, rate_div, tracks, bytes; };
    int bk_peek_tracks(const uint8_t *data, uint32_t len);   /* audio tracks from the header; -1 not a Bink header */
    struct bk_movie *bk_open(const uint8_t *data, uint32_t len, const char **err);
    const struct bk_info *bk_info_of(const struct bk_movie *m);
    void bk_goto(struct bk_movie *m, uint32_t frame);   /* 1..frames: the frame bk_frame gives next */
    uint32_t bk_current(const struct bk_movie *m);
    int bk_frame(struct bk_movie *m);                   /* decode the current frame once; 0 on a decoder error */
    int bk_copy(const struct bk_movie *m, uint8_t *dst, uint32_t pitch, uint32_t dst_h,
                uint32_t x, uint32_t y, uint32_t type); /* 0 done; -1 type not supported */
    uint32_t bk_decoded(const struct bk_movie *m);      /* frames decoded since bk_open (checks) */
    void bk_close(struct bk_movie *m);
    ```
  - `bk_open` reads the 44-byte header itself first. It fails with `*err` set (and returns NULL) for a magic other than `BIK` plus a revision letter, a declared size (`Size + 8`) larger than `len`, zero frames, or a zero width or height. `bk_peek_tracks` reads the track count at header offset 40 from the first 44 bytes alone (the same magic check, no FFmpeg), so the guest layer can stop on a track before anything is opened. The bytes are read in place through an `AVIOContext` with read and seek callbacks over `data`. Nothing is copied, and the caller keeps `data` valid until `bk_close` (Ruling 4).
  - Goto (Ruling 5): the same frame does nothing. A later frame decodes the frames in between and drops them. An earlier frame rewinds to the first frame (`av_seek_frame` to 0, `avcodec_flush_buffers`) and decodes forward. `bk_frame` decodes the current frame only if it is not already the decoded one.
  - Copy (Ruling 6): `type` 3 (`BINKSURFACE32`, X8R8G8B8 in memory as B, G, R, 0xFF) and 5 (`BINKSURFACE32A`, the movie's alpha plane or 0xFF). It writes rows `y .. min(y + height, dst_h) - 1` and columns `x .. min(x + width, pitch / 4) - 1`, nothing else. YUV to RGB uses BT.601 in the range the decoder reports (`AVCOL_RANGE_JPEG` full, otherwise limited, which FFmpeg reports for revision `i`). Any other type gives -1.

- [ ] **Step 1: Write the failing tests.**
  - `test_ffmpeg_prefix` (`tests/test_build.py`): calls `ffmpeg_prefix()` and `engine_libs()` directly, with `BW2_FFMPEG_PREFIX` set by `monkeypatch.setenv`. A temporary prefix missing `libavcodec.dylib` raises `ToolsMissing` naming it. The real prefix gives the link flags above. With the variable pointing at an empty folder, `import bw2.build` (already imported) still works and `HOST_LIBS["linkcheck"]()` gives `[]`, so only an engine build needs FFmpeg. `test_engine_ffmpeg_links` (after a build): `otool -L out/bw2` (otool from the developer directory `check_tools()` finds) lists exactly three FFmpeg libraries, `libavformat`, `libavcodec` and `libavutil`, and no `libswscale`, `libswresample`, `libavfilter`, `libavdevice` or `libpostproc`. Each listed install name exists on disk; a missing one fails naming it and saying a `brew upgrade ffmpeg` needs a rebuild of `out/bw2`. The test records each install name with its current and compatibility versions (`record_property`, and Task 8 copies them into `campaign.md`).
  - `video_selftest.c` groups (the harness of `tests/test_audio.py`: engine sources plus the selftest, one group per run, `<group>: 0 failed`). Groups that need a movie take its path, and for the archive an offset and a length, as arguments. `tests/test_video.py` passes them from the game tree and carries `gamefiles`:
    - `bink_header` (no game files): hand-made 44-byte headers. Magic `XYZi` fails `bk_open` with "not a Bink movie", and `bk_peek_tracks` gives -1. A declared size larger than `len` fails with "truncated". A header declaring one audio track gives `bk_peek_tracks == 1`, and 0 tracks gives 0;
    - `bink_ealogo` (`Data/Splash/EAlogocompressed.bik` read into memory): `720x540`, 120 frames, rate 30/1, `bk_peek_tracks == 1` and `tracks == 1`, 644,220 bytes. Its audio track is never opened, and the video decodes all the same. `bk_goto(1)` then `bk_frame` gives 1 decoded. `bk_goto(60)` then `bk_frame` gives 60 decoded. `bk_goto(60)` again then `bk_frame` still gives 60. `bk_goto(10)` then `bk_frame` gives 70 decoded, and frame 10's copy equals the copy taken at frame 10 on a first pass (byte for byte);
    - `bink_last_frame`: `bk_goto(120)` and `bk_frame` 1,000 times decode once, and each copy equals the first;
    - `bink_copy_clip`: a type-3 copy into a buffer of half the movie's height at `x = 2`, with 64 guard bytes before and after and a pitch of `width * 4 + 16`, writes only inside `[y, dst_h)` rows and `[x, pitch / 4)` columns. The guards are untouched, and every written alpha byte is 0xFF. Type 7 gives -1;
    - `bink_archive`: the attract (`Everything.stuff` at `0x30D3C440`, 32,540,316 bytes) and the intro (`0x3B02BD00`, 51,782,036 bytes), each mapped read-only by the selftest: header values as in "Measured" item 2, and every frame from 1 to `frames` decodes in order with no decoder error;
    - `bink_two_movies`: the attract and the EA logo open at once, interleaved goto/frame/copy on both, and each one's frame 30 copy equals its copy when decoded alone.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_build.py tests/test_video.py -v` (and with `BW2_GAMEFILES=1`). Expected: FAIL.
- [ ] **Step 3: Implement `ffmpeg_prefix` and the build changes, then `bink_dec.c`**, adapting pcrecomp's `win32hle/bink.c` (MIT; a line in the file says so). Audio streams are never opened.
- [ ] **Step 4: Run them, then `.venv/bin/python -m bw2.pipeline --build-only` and the engine's quick tests** (`tests/test_engine.py tests/test_build.py -q`). Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Decode Bink movies with FFmpeg"`

### Task 6: The binkw32 imports, the Bink log and the movie quit switch

**Files:**
- Create: `runtime/bw2/video/binkw32.c`.
- Modify: `runtime/bw2/later.c` (`binkw32.dll` leaves `g_later_dlls`), `runtime/bw2/register.c` (`bw2_video_register`), `runtime/bw2/d3d9/d3d9_device.c` (`present_bb` calls `bw2_movie_present`), `runtime/bw2/video/video_selftest.c`, `tests/test_video.py`.

**Interfaces:**
- Consumes: Task 5's `bk_*` functions.
- What the game reads (disassembly of `white.exe`, 2026-11-13). These drive the interface below:
  - `sub_0093C7B0`, right after `_BinkOpen@8` (`0x93C82E`), reads the handle's `+0x14` and `+0x18` (`FrameRate`, `FrameRateDiv`; it stores `rate / div` as its fps), `+0x8` (`Frames`; it stores `Frames / fps` as the movie's length), then `+0x4` and `+0x0` (`Height`, `Width`), which it hands to `sub_0093C740` to make the texture. These five fields must be exact.
  - `sub_0093C460`, once per rendered frame, calls `_BinkGoto@12(h, int(fps * t) + 1, 0)`, where `t` is clamped below at 0.0 (`0x93C485`-`0x93C499`) and never above, then `_BinkDoFrame@4(h)`, `GetSurfaceLevel(0)` (texture vtable `+0x48`), `_BinkDX9SurfaceType@4(surface)`, `LockRect` (surface `+0x34`; on failure it skips the copy), then `_BinkCopyToBuffer@28(h, bits, pitch, h->Height (+0x4), 0, 0, type)`, where `type` is the surface-type result itself, then `UnlockRect` (`+0x38`) and `Release` (`+0x8`).
  - So the Copy flags always equal `_BinkDX9SurfaceType@4`'s answer. A surface format binkw32 does not map gives `0xFFFFFFFF`, and that arrives at Copy as flags `0xFFFFFFFF`, which stops loudly there, naming the flags. Nothing is drawn silently wrong.
  - No other field is read by these two functions. The rest of the zeroed 0x400-byte block is documented padding: Bink 1.x's later fields are not filled, and a read of one would show as a zero the game acts on. Task 8's research run watches for that.
- Produces:
  - `void bw2_video_register(void)` (from `register.c`, after `bw2_audio_register`) and `void bw2_movie_present(void)` (from `present_bb`, after the frame log). Both are declared `extern` where called;
  - `_BinkOpen@8(p, flags)`: `flags` must be `0x04000000` (anything else stops naming it). `p` points at the movie in guest memory. `bk_peek_tracks(p, 44)` comes first: one audio track or more calls `bw2_later_load("Bink audio track")` (exit 4), and -1 stops as a failed open. The length handed to `bk_open` is the header's `Size + 8`, which binkw32 trusts the same way (the game keeps the whole movie mapped). A failed open stops naming the reason and `p`. On success it returns a guest block (`hle_alloc`, 0x400 bytes, zeroed) holding Bink 1.x's leading fields: `Width` +0, `Height` +4, `Frames` +8, `FrameNum` +0x0C (1 after open), `LastFrameNum` +0x10, `FrameRate` +0x14, `FrameRateDiv` +0x18, `OpenFlags` +0x20, `Size` +0x28 (the header's). The `struct bk_movie *` lives in a host table keyed by that address (Ruling 8), never in guest memory;
  - `_BinkGoto@12(h, frame, flags)`: does what binkw32 1.8's own `_BinkGoto@12` (`0x18011A60`) does with the frame: 0 becomes 1 (`test ebx, ebx; jne; mov ebx, 1`), and a frame above `Frames` becomes `Frames` (`cmp ebx, [esi+8]; jbe; mov ebx, eax`). The game asks `Frames + 1` and more once the movie's time has passed its end, because its `sub_0093C460` has no upper clamp. `flags` other than 0 stop loudly (the game passes 0). It does `bk_goto` with the clamped frame and sets `FrameNum` to it. It returns nothing (pops 12 bytes). The largest frame asked before clamping is kept per handle for the close log;
  - `_BinkDoFrame@4(h)`: `bk_frame` (a decoder error stops naming the frame), sets `LastFrameNum`, and returns 0;
  - `_BinkCopyToBuffer@28(h, dst, pitch, height, x, y, flags)`: `flags` must be 3 or 5 (any other bit or type stops naming it; this is also where an unmapped surface format, `0xFFFFFFFF` from `_BinkDX9SurfaceType@4`, stops). It does `bk_copy` and returns 0;
  - `_BinkDX9SurfaceType@4(surface)`: binkw32 1.8's mapping ("Measured" item 4) from our surface's format. NULL and any other format give `0xFFFFFFFF`, as binkw32 does;
  - `_BinkClose@4(h)`: `bk_close`, frees the block, forgets the handle. An unknown handle passed to any of these stops naming it;
  - `BW2_BINK_LOG=1` (read once) prints numbers only: `[bw2] bink open: <w>x<h>, <frames> frames at <rate>/<div> fps, <tracks> audio tracks, <bytes> bytes from memory`, `[bw2] bink last frame: <frames>` the first time a movie's last frame is copied, and `[bw2] bink close: <decoded> frames decoded, largest goto <n>` (`n` before the clamp);
  - `BW2_QUIT_AFTER_MOVIE=<frames>:<frame>[+<presents>]` (read once, at the first Bink call or Present): `<frames>` names the movie by its frame count (2,033 for the attract, 1,798 for the intro), so the attract can never end a test meant for the intro. Once a `_BinkCopyToBuffer@28` has copied a frame numbered `>= frame` in an open movie whose `Frames` is `<frames>`, the `presents`-th following `Present` (or that copy itself when `presents` is 0 or absent) prints `[bw2] quit at movie frame <frames>:<frame> + <presents> presents (BW2_QUIT_AFTER_MOVIE)` and calls `hle_exit(0)`. A malformed value stops at its first read, naming it.

- [ ] **Step 1: Write the failing checks** (`video_selftest.c`, calling the shims through `call()` as `shims_selftest.c` does, on a headless device as `d3d9_selftest.c` makes one):
  - `binkw32_open` (the attract's 32,540,316 bytes from `Everything.stuff` at `0x30D3C440`, copied into an `hle_alloc` block): the returned block's fields at +0, +4, +8, +0x0C, +0x14, +0x18, +0x20 read 720, 576, 2033, 1, 25, 1, `0x04000000`. With `BW2_BINK_LOG=1` the open line reads `[bw2] bink open: 720x576, 2033 frames at 25/1 fps, 0 audio tracks, 32540316 bytes from memory`;
  - `binkw32_frame`: Goto 5, DoFrame, then CopyToBuffer into a locked X8R8G8B8 720x576 texture's surface gives 0. The texture read back is not all black and equals `bk_copy` of frame 5. `FrameNum` and `LastFrameNum` read 5. A surface whose format binkw32 does not map (`D3DFMT_DXT1`) gives `_BinkDX9SurfaceType@4 == 0xFFFFFFFF`, and passing that as Copy's flags, as the game would, stops naming `0xFFFFFFFF` (child);
  - `binkw32_goto_clamp`: Goto 0 then DoFrame gives `FrameNum == 1` and a copy equal to frame 1's. Goto 2034 and Goto `0x7FFFFFFF` then DoFrame give `FrameNum == 2033`, a copy equal to frame 2033's, and no read past the movie's bytes (for this check the movie is copied to the end of a `VirtualAlloc` region whose next page is unreserved, so a read past it faults). Goto 2033 after Goto 2034 decodes nothing new. The close line reports `largest goto 2147483647`;
  - `binkw32_surface_type`: surfaces of formats 20 to 26 and 30 give 1, 5, 3, 10, 9, 8, 7, 7. NULL and a `D3DFMT_DXT1` surface give `0xFFFFFFFF`;
  - `binkw32_reopen`: open, close and open again from a new copy of the bytes: the second handle starts at frame 1 and decodes;
  - `binkw32_loud` (children): open flags `0x00800000`, Goto flags 1, Copy flags `0x80000003` and `0xFFFFFFFF`, a handle never opened, and the EA logo's bytes, which carry one audio track (exit 4 with `[bw2] reached Bink audio track: owned by a later plan`). Each stops naming what it got;
  - `binkw32_quit` (child): with `BW2_QUIT_AFTER_MOVIE=2033:3+2`, the intro (`0x3B02BD00`, 51,782,036 bytes) opened and frames 1 to 5 copied, then the attract opened and frames 1 to 3 copied, then two Presents: the intro's copies do not arm the quit, and the run exits 0 with the quit line at the second Present after the attract's frame 3. `BW2_QUIT_AFTER_MOVIE=x` and `BW2_QUIT_AFTER_MOVIE=100` (no movie named) each stop naming the value.
- [ ] **Step 2: Run them.** `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_video.py -k binkw32 -v`. Expected: FAIL.
- [ ] **Step 3: Implement `binkw32.c`**, remove `binkw32.dll` from `g_later_dlls` (its comment too), and hook `register.c` and `present_bb`. Then relink: `.venv/bin/python -m bw2.pipeline --build-only`.
- [ ] **Step 4: Run them, then `tests/test_engine.py tests/test_d3d9.py -q`.** Expected: all pass. `tests/test_boot.py`'s Bink test now fails; Task 8 rewrites it.
- [ ] **Step 5: Commit.** `git commit -m "Answer the game's Bink calls"`

### Task 7: Uploads during a long load, and the frame dump

**Files:**
- Modify: the Metal upload path in `runtime/bw2/d3d9/mtl_device.m` / `mtl_buffer.m` / `mtl_internal.h` (or wherever Step 1 finds the cause), `runtime/bw2/d3d9/d3d9_device.c` (the dump call in `present_bb`).
- Create: `runtime/bw2/d3d9/frame_dump.m`.
- Test: `runtime/bw2/d3d9/d3d9_selftest.c`, `tests/test_d3d9.py`.

**Where things live today** (read 2026-11-13): `mtl_tex_upload` (`mtl_device.m`, near line 302) copies each upload into a new shared `MTLBuffer` and encodes a blit into the one open command buffer (`mtl_device.m:127-150`), which is committed only at `Present` or `mtl_flush`. A managed texture's lock copy is guest memory: `d3d_tex_mem` (`d3d9_texture.c:32`) takes it from `hle_alloc`, once per level, and keeps it for the texture's life. So a test that keeps many large textures alive spends the guest's roughly 2 GB below `0x7F000000`, and a 1.5 GB flood could fail for that reason alone.

**Interfaces:**
- Produces:
  - the fix for the cause Step 1 measures. If that is the hypothesis (staging buffers piling up in one open command buffer), the bound is: the open command buffer is committed and a new one begun once its staged upload bytes pass 64 MB (Ruling 13). If Step 1 finds a different cause, the fix follows the cause, and Ruling 13 and Task 8's doc say so;
  - `BW2_FRAME_DUMP=<dir>[:<every>]` (read once; `every` defaults to 100): every `every`-th Present writes `<dir>/frame-<n>.png` (the back buffer, RGB, through ImageIO) and prints `[bw2] frame dump: <n>`. A `<dir>` inside a git work tree (any ancestor holds `.git`) or not creatable stops at the first Present, naming it. Ruling 14.

- [ ] **Step 1: Measure the failure in the real boot, first.** On a scratch branch or with a temporary counter, run the land load ("Measured" items 5 and 6: the profile script plus the New Game click) and record, for the failing command buffer: the uploads and staged bytes it carries, the blits and draws in it, and how long it has been open. At the same moment record the candidates that could also exhaust memory, so each is ruled in or out by a number: the live count of `MTLBuffer` staging objects and their bytes; the live count of our D3D9 surface objects and Metal textures (the per-frame new `GetSurfaceLevel` objects of "Measured" item 3 could be a leak, if a released surface keeps its Metal texture or its lock copy); the process's resident and GPU-allocated size (`[MTLDevice currentAllocatedSize]`); and the guest heap's size. Record what the numbers say (the commit message of Step 5 and Task 8's doc). Remove the counter.
- [ ] **Step 2: Write the failing checks,** shaped by Step 1's mechanism.
  - `mtl_upload_flood` (`d3d9_selftest.c`): reproduces the measured mechanism with the guest footprint kept under 256 MB. For the staging hypothesis: with no Present, a loop creates a managed 1024x1024 A8R8G8B8 texture, fills it with its own solid colour (`LockRect`/`UnlockRect`) and releases it, so at most a few are alive at once (Metal retains what the open command buffer uses). Every 32nd texture is kept alive (at most 64 MB of lock copies in all) and is drawn into a 1x1 render target and read back at the end. No `[bw2] metal: command buffer failed` line appears on stderr, and every kept texture reads its colour. Before the fix, find the smallest count that fails on this Mac (start from the staged bytes Step 1 measured, double until it fails) and record it in the check's comment; the check runs at twice that count. If Step 1 shows a leak instead (objects that outlive their `Release`), the check is a loop of that pattern (for example `GetSurfaceLevel`, `LockRect`, `UnlockRect`, `Release` on one texture, as `sub_0093C460` does each frame) that asserts the live object count and allocated size stay flat.
  - `frame_dump` (child): with `BW2_FRAME_DUMP=<tmp>:2`, four Presents of a cleared 64x48 back buffer (red, green, blue, white) write `frame-2.png` and `frame-4.png`, 64x48, whose centre pixels are green and white. `<tmp>` inside a folder holding `.git` stops naming the folder.
- [ ] **Step 3: Run them.** `.venv/bin/pytest tests/test_d3d9.py -k "flood or dump" -v`. Expected: FAIL.
- [ ] **Step 4: Implement the fix and `frame_dump.m`.**
- [ ] **Step 5: Run them, then `tests/test_d3d9.py tests/test_shaders.py -q`.** Expected: all pass. `git commit -m "Bound the GPU work queued between frames"` (or a subject naming the measured fix), then `git commit -m "Add the PNG frame dump switch"`

### Task 8: The boot to the attract and into the campaign, measured

**Files:**
- Create: `docs/research/campaign.md`.
- Modify:
  - `tests/test_boot.py`: `test_boot_profile_to_bink` becomes `test_boot_profile_to_attract`; new `NEW_GAME_SCRIPT` and `test_boot_new_campaign`; `BINK_STOP` goes; the module docstring;
  - the spec: 5.2 Video (a plan 10 note), 5.2 D3DX (DXT compression and parameter blocks), 7.3 (the frame dump), 9 (FFmpeg as separate dynamic libraries, Ruling 2), 11 "Left to the builder" (a Bink decoder line pointing to Ruling 2);
  - `docs/research/hlsl.md`: a pointer from its "For plan 10" paragraph to `campaign.md`.

**Interfaces:**
- Consumes: everything above.
- Produces: the measured record the next plan starts from.

- [ ] **Step 1: Write the failing tests.**
  - `NEW_GAME_SCRIPT = PROFILE_SCRIPT + "300 move -170 284\n310 down 0\n314 up 0\n"` ("Measured" item 5).
  - `test_boot_profile_to_attract` (gamefiles; `PROFILE_SCRIPT`, `BW2_FRAME_LOG=1`, `BW2_HLSL_LOG=1`, `BW2_BINK_LOG=1`, `BW2_QUIT_AFTER_MOVIE=2033:100`, timeout 300 s). It asserts:
    - exit 0 and the quit line for movie frame 2033:100 + 0 presents;
    - exactly one bink open line, `[bw2] bink open: 720x576, 2033 frames at 25/1 fps, 0 audio tracks, 32540316 bytes from memory`;
    - no `LOUD` line, and no `owned by a later plan` line;
    - the profile files, the `[module]` set, the lifted D3DX line and the one HLSL effect as the old test pinned them;
    - the order: the first read, the effect, the bink open, the quit line;
    - among the frames after the bink open, at least one with a non-black share at or above a bound Step 3 sets (at most half the smallest value measured over three runs).
  - `test_boot_new_campaign` (gamefiles; `NEW_GAME_SCRIPT`, `BW2_FRAME_LOG=1`, `BW2_BINK_LOG=1`, `BW2_QUIT_AFTER_MOVIE=1798:<L>+600`, where `L` is the intro's last frame number as Step 3 measures it (at most 1,798, since Goto clamps there), timeout 900 s). It asserts:
    - exit 0 and the quit line;
    - exactly one bink open line, `[bw2] bink open: 640x360, 1798 frames at 5000000/208333 fps, 0 audio tracks, 51782036 bytes from memory`, and the `bink last frame` line if Step 3 finds the game shows the last frame;
    - no `LOUD` line, no `owned by a later plan` line, and no `command buffer failed` line naming `Insufficient Memory`. Lines naming `Impacting Interactivity` are allowed as in the profile test, and their frames are left out of the frame checks;
    - no file under `<support>/Game/`, and `[module]` lines only from the known set (extended only by lines Step 3 finds and explains in `campaign.md`);
    - at least one frame during the movie (between the open line and the last-frame line) with a non-black share at or above Step 3's movie bound;
    - the last 100 frames before the quit line each at least half non-black.
- [ ] **Step 2: Run them.** `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -k "attract or campaign" -v`. Expected: FAIL until the bring-up below is done.
- [ ] **Step 3: Bring-up loop.** `BW2_HEADLESS=1 BW2_FRAME_LOG=1 BW2_BINK_LOG=1 BW2_INPUT_SCRIPT=<script> BW2_SUPPORT=$(mktemp -d) out/bw2 --game gamefiles/game > <scratch>/boot.err 2>&1`, reading the tail. `BW2_FRAME_DUMP` points at a scratch folder outside the repo when a picture is needed; delete the PNGs afterwards.
  - Each stop in this plan's layers is fixed in the owning task's file, with its check first, red then green.
  - A missed function start goes to the catalog as a `"boot"` start (or `"data"` if `data_starts` should have found it, and then `data_starts` is fixed), with a full pipeline run.
  - A stop at a later plan's name is recorded, and the campaign test's expected end becomes that stop (exit 4).
  - A stop outside every plan's layer is fixed here if it is a Win32 answer of the kind Task 2 makes (with a `shims_selftest.c` check first). Anything larger is recorded and goes back to the planner.
  - Any disc, volume or driver question, or a tripwire, stops the work and goes to the user with the evidence.
  - The repeated folder ("Measured" item 7): find the code that builds that name. If one of our answers (path, string or CRT) makes the repeat, fix it with a check first. If the game builds it so from its own data, record it as the game's.
  - Measure, over three runs each (one with a fresh support folder): the attract's open frame and time, and the non-black share of its frames. For the campaign: the time from the click to the intro's open, the intro's last frame number `L` (does the game reach 1,798?), the largest `n` the game asks `_BinkGoto@12` for in each movie (the close line's `largest goto`, or the last asked if a movie is never closed: so the clamp's use is on record), whether it closes the intro and when, the non-black share during the movie and of the last 100 frames, and the land-load time the game's debug output reports. Set the tests' bounds from these, each at most half the smallest value measured.
- [ ] **Step 4: Write `docs/research/campaign.md`** (facts only; the game's own output described, never quoted). It records:
  - the Bink path: the attract's name and its fallback to the archive, the view and the open, the per-frame call order with addresses ("Measured" items 1-4), the struct fields the game reads as Task 6 pinned them (and any further field Step 3 sees it read), the largest Goto frame per movie and how often the clamp applied, the archive's 122 movies by size and track count, and the 22 with a track still a later-plan stop;
  - the New Game path: the script, the click, each stop of "Measured" item 5 and its fix, the land-load time, and Task 2's finding on what `sub_006139E0` does with `HeapCompact`;
  - the Metal finding (Task 7 Step 1) and the fix;
  - the FFmpeg install names and versions `test_engine_ffmpeg_links` recorded, and the note that this GPL build never leaves the Mac;
  - the repeated folder's answer;
  - frames per second during the movie and in the land's opening scene, headless;
  - the `GetSurfaceLevel` observation ("Measured" item 3) for a later D3D9 plan;
  - the absence of any disc, volume or driver question, tripwire or VM import over the whole campaign run;
  - for the next plan: where the boot test now ends and what the opening scene waits for (from a `BW2_TRACE_IMPORTS=1` run: imports first seen after the intro).
- [ ] **Step 5: Update the docs.**
  - **Spec 5.2 Video:** a "Plan 10" note: the six imports answered from `runtime/bw2/video/` (adapted from pcrecomp's `bink.c`), movies read in place from the game's view of `Everything.stuff` (`BINKFROMMEMORY`), FFmpeg's libavformat, libavcodec and libavutil as separate dynamic libraries, Goto semantics, binkw32 1.8's surface-type mapping, and audio tracks left as a later-plan stop.
  - **Spec 5.2 D3DX:** a "Plan 10" note: DXT1/3/5 compression for loads, fills and mip chains, and effect parameter blocks.
  - **Spec 7.3:** the frame-capture sentence gains "`BW2_FRAME_DUMP` writes PNG frames to a folder outside any git tree (plan 10)".
  - **Spec 9:** the licence line names FFmpeg (LGPL-2.1+) as a separate dynamic library, and the shipped app's minimal LGPL build (Ruling 2).
  - **Spec 11 "Left to the builder":** a line "Bink decoder (plan 10, Ruling 2: FFmpeg's, as separate LGPL dynamic libraries)".

  Then run the full suite: `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan10-suite.txt 2>&1; tail -3 .superpowers/sdd/plan10-suite.txt`. Expected: all pass.
- [ ] **Step 6: Audit and commit.** `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through the intro movie into the first land"`

## Rulings (my calls; the spec leaves them to the builder)

1. **Scope and phasing:** plan 10 answers Bink video and everything the New Game path needs up to the first land's opening scene (the five answers of "Measured" item 5, and the Metal bound). Bink audio tracks wait: neither the attract nor the intro has one, binkw32 picks its sound output at run time through `LoadLibraryA` ("Measured" item 4), and no run has opened a movie that carries one (22 in the archive, and the EA logo). Opening one exits 4 as a later plan's layer. Input on the land, saves and the rest of the campaign are later plans.
2. **FFmpeg, as the spec planned,** as separate dynamic libraries (LGPL-2.1+, spec 9), and only `libavformat`, `libavcodec` and `libavutil`; a test pins that set. Development links Homebrew's `ffmpeg` 9.0.2, already installed on this Mac (Homebrew use recorded here). It is a GPL-enabled build, which is fine for a local engine that is never distributed: `out/bw2` and anything linked against it never leave this Mac, and no test or script copies them elsewhere. A `brew upgrade ffmpeg` that changes a library's major version breaks `out/bw2` until it is rebuilt; `test_engine_ffmpeg_links` names the missing library and records the linked versions. The app the Builder makes (a later plan) instead bundles a minimal LGPL-only FFmpeg it builds from the pinned `ffmpeg-9.0.2.tar.xz` (SHA-256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`). That build enables only the Bink demuxer and the Bink video and audio decoders, with no `--enable-gpl` and no `--enable-nonfree`. `BW2_FFMPEG_PREFIX` selects it. No FFmpeg source enters the repo. A clean-room Bink decoder was not chosen: it is weeks of work with no reference to check it against.
3. **The layer lives in `runtime/bw2/video/`, adapted from pcrecomp's MIT `bink.c`,** not as a vendored patch, because its sound (win32hle's SDL audio) and surfaces (DirectDraw) are not ours.
4. **`BINKFROMMEMORY` only, read in place.** Every measured open passes a pointer into the game's own view with flags `0x04000000`. The decoder reads the guest's bytes where they are, with no copy, and the game keeps the view mapped while the movie is open, as binkw32 requires. Other flags (file names, `BINKFILEHANDLE`) stop loudly until a run shows them.
5. **Goto:** frame 0 becomes 1 and a frame above `Frames` becomes `Frames`, as binkw32 1.8's own `_BinkGoto@12` does (`0x18011A60`; measured code, like Ruling 7), because the game's `sub_0093C460` asks past the end once a movie's time is up. Only nonzero flags and unknown handles stop. The same frame is free; forward decodes and drops the frames in between, as `_BinkGoto@12` without `BINKGOTOQUICK` does; backward rewinds to frame 1. The game only goes forward ("Measured" item 3), and FFmpeg's Bink demuxer seeks only to the start, so a keyframe index would buy nothing measured.
6. **Colour:** BT.601 in the range FFmpeg's decoder reports for the movie (limited for revision `i`). No Windows reference exists to check binkw32's own conversion (spec 7.3), and FFmpeg's choice is the best-documented one. The X byte of `BINKSURFACE32` is written as 0xFF.
7. **`_BinkDX9SurfaceType@4`** returns binkw32 1.8's own mapping, read from its code ("Measured" item 4), including `0xFFFFFFFF` for NULL and unknown formats. That is the DLL's documented answer, not a silent one.
8. **Host state off the guest heap:** the guest block holds only Bink's public fields, and our decoder state is in a host table keyed by the block's address. The game cannot overwrite a host pointer, and a stale or foreign handle stops loudly.
9. **DXT compression is ours** (`d3dx_dxt.c`, MIT), not the lifted `d3dx9_25.dll`'s. The textures belong to our native D3DX and device. Microsoft's exact compressor bits cannot be checked without a Windows reference. A principal-axis BC1 fit with a least-squares refit is within a few levels of D3DX's quality. DXT1's transparent texels follow alpha < 0.5, as D3DX's one-bit alpha does. DXT2 and DXT4 (premultiplied) stop loudly: the game was not seen to use them.
10. **Parameter blocks defer, as native D3DX does.** While a block records, a setter writes only into the block, and the parameter's live value, its dirty mark and its texture reference stay as they were until `ApplyParameterBlock` writes the records in order and marks them dirty. The reference is Wine's d3dx9 `effect.c` (`param_get_data_and_dirtify`, `record_parameter`, `set_value`, `free_parameter_block`), whose parameter-block test was written against native d3dx9. A recorded but unapplied texture is held by the block (one reference per record) and not by the live slot. Applying at once would leave the values of the last of the 10 or more blocks `sub_00963C80` records live and rendered, and that would be wrong. Blocks live until the effect is released (d3dx9_25 has no `DeleteParameterBlock`). Misuse the game has not shown stops loudly instead of guessing D3DX's return codes.
11. **`VirtualLock`** checks (through `hle_vm_is_reserved` over win32hle's reservation table, which holds heap and `VirtualAlloc` memory alike) and remembers ranges but pins nothing on the host: guest memory is never paged out under the game, so the lock's only observable effect is its return value. **`HeapCompact`** reports the largest range the guest could still reserve, capped at the 1.5 GB `GlobalMemoryStatus` already reports, so the game's two memory questions agree.
12. **Catalog fixes stay data-driven:** the backward table slots are read only after `neg` of the index register (MSVC's memcpy and memmove `CopyUnwindDown`, 4 sites), by every reader of jump tables (disasm32's function walk, `body_instructions` and `close_dispatch_targets`, and discovery's `_decoded_marks`), through one `negated_index` helper so they cannot disagree. The data-named starts are recorded with reason `"data"`, pinned to the 7 that 4-byte-aligned dwords in `.rdata` and `.data` name; the 3 extra hits a scan of every non-executable section at every offset gives are not starts (Task 1). Discovery's own extents are not reworked here; the overshoot behind `0x746700` is recorded in `campaign.md`.
13. **The GPU bound:** 64 MB of staged upload per command buffer, if Task 7 Step 1's measurement confirms that uploads piling up in one open command buffer are the cause (the code supports it: `mtl_device.m:127-150` commits only at Present or flush). If it shows another cause (a leak of surfaces or staging buffers, for one), the fix follows the measurement and this ruling is rewritten. A loading screen with no Present for 23 s must not pile every texture of a land into one buffer.
14. **`BW2_FRAME_DUMP`** is spec 7.3's "make frame capture easy": PNG through ImageIO, a folder outside any git work tree (refused otherwise), so captured frames, which are game material, can never be committed by accident. The boot tests do not use it.
15. **`BW2_QUIT_AFTER_MOVIE` and `BW2_BINK_LOG`** are test switches like `BW2_QUIT_AFTER_READS`. Once the attract plays there is no stop to end a run, and frame and read counts depend on loading speed (`hlsl.md`). A movie frame number is set by the movie's clock and marks the same moment in every run. The switch names the movie by its frame count, so a frame of a different movie never ends the run. The log prints numbers only.
