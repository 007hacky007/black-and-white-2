# Bink movies and the boot into the campaign (plan 10)

Facts about the boot from the front end's attract video and from a click on New Game, through the intro
movie, to the first land's opening scene, as the engine at the end of plan 10 runs it (headless, on a
32 GB Apple Silicon Mac, 2026-11-13). Addresses are `white.exe`'s. Nothing here quotes the game's strings,
data or output: a debug line or a data string is described in neutral words.

Runs behind the numbers: the attract three times (`PROFILE_SCRIPT`, two with a fresh support folder, one
reusing a folder that already held the profile), the campaign three times (`NEW_GAME_SCRIPT`, two fresh,
one reusing), one campaign run with `BW2_TRACE_IMPORTS=1` and `HLE_FILETRACE=1`, and a few runs with
temporary prints (a read-count mark, Goto counters, path and stack dumps), all removed before the commit.
The boot tests' bounds are set from these runs (each at most half the smallest value measured).

## The Bink path

**The movie's bytes.** The front end's idle timer (about 30 s after the profile's effect: 30.1, 30.1
and 33.1 s in three runs) makes the game's CRT file open (`sub_00AEF0C8`, `CreateFileA`) ask for the
attract video as a loose file in `Data/Splash`, which no install holds. The game then maps a view of
`Data/Everything.stuff` (`MapViewOfFile` from `sub_009240C0`) and calls `_BinkOpen@8` from
`sub_0093C7B0` with a pointer to the movie inside the view and flags `0x04000000` (`BINKFROMMEMORY`).
The decoder reads the movie in place, only its declared Size + 8 bytes (Task 5). The New Game path opens
the intro the same way: the CRT open asks for it as a loose file in the `Data/Tutorial AVI` folder, which
no install holds, and the game maps its view of the archive and opens it from there.

**The archive.** `Data/Everything.stuff` (1,427,446,404 bytes) holds 122 Bink 1 movies, all revision
`i` with no alpha: 94 at 400x300, 23 at 512x384, 3 at 720x576, 1 at 640x360 and 1 at 320x240. 100 have
no audio track and 22 have one. Opening one of those 22 still exits 4 (`reached Bink audio track`, a
later plan's layer, Ruling 1); no run in this plan opened one, and the loose EA logo movie (one track) is
never opened.

| Movie | Offset in the archive | Bytes | Size | Frames | Rate | Tracks |
| --- | --- | --- | --- | --- | --- | --- |
| Attract | `0x30D3C440` | 32,540,316 | 720x576 | 2,033 | 25/1 | 0 |
| Intro | `0x3B02BD00` | 51,782,036 | 640x360 | 1,798 | 5000000/208333 (24.0) | 0 |

**After the open** the game makes the movie's texture (`D3DXCreateTexture`, thunk `sub_00B522F4`) and
reads its level (`GetLevelDesc`, `sub_0093C5D0`). Then once per rendered frame `sub_0093C460` calls, in
this order: `_BinkGoto@12(h, n, 0)`, `_BinkDoFrame@4(h)`, `IDirect3DTexture9::GetSurfaceLevel`,
`_BinkDX9SurfaceType@4(surface)`, `IDirect3DSurface9::LockRect`, `_BinkCopyToBuffer@28(h, bits, pitch,
height, 0, 0, 3)`, `UnlockRect` and `Release`. The pitch and height are 2,880 and 576 for the attract,
2,560 and 360 for the intro. `n` follows the movie's clock (`n = int(fps * t) + 1`): the same `n` is
asked many times at the headless frame rate, it jumps when frames are slow, and it never goes back.
`_BinkNextFrame@4` and `_BinkWait@4` are not imported, and `_BinkClose@4` is the only other call.

**The fields the game reads** from the handle are those Task 6 pinned (Width, Height, Frames, FrameRate
and FrameRateDiv, read in `sub_0093C7B0`). The runs here saw no stop or wrong picture that would point
to another field; no run traced the game's reads of the block beyond that.

**Goto past the end.** Every run played both movies to their last frame (the attract to 2,033, the
intro to 1,798; the spike behind the plan saw the attract stop at 2,020 with its logging stubs). In every
run the largest frame the game asked was the last frame + 1 (2,034 and 1,799), asked exactly once, and
that was the movie's last `_BinkGoto@12`: the clamp of Ruling 5 applied once per movie (counted in the
three attract runs and two of the intro runs). After it the game
makes no Bink call for that movie until it closes it:

- the attract is closed 111.3 s after its open in all three runs (30 s after its last frame) and opened
  again at once from a new view;
- the intro is never closed: runs of 600 s and 352 s (the open at 18 and 45 s) showed no close.

Over a whole attract the game made 4,011 to 11,957 `_BinkGoto@12` calls (one per rendered frame, so the
count follows the frame rate); the traced campaign run, slowed by its tracing, made 1,801 for the intro.

**The attract, measured** (three runs): the open came after front-end frame 812, 635 and 1,122 (frame
numbers count every Present from the first). Some frame within the first 4 s after the open (movie frames
up to 100, where the test quits) was fully non-black in every run, so the profile test's bound is half the
picture. Over the whole movie the darkest frame was 0 to 48% non-black (the movie has dark scenes).

## The New Game path

**The script** is `PROFILE_SCRIPT` plus a move of (-170, 284) at read 300 and a click at reads 310 and
314: the running point goes from (422, 427) to (252, 711), the front end's New Game button at 1024x768,
before the attract's timer. The click came 12.3 to 26.0 s after the start (35.3 s in the traced run).

**The stops, in the order the plan's spike met them, and their fixes:**

| Stop | Where | Fix |
| --- | --- | --- |
| `VirtualLock` unimplemented | `(0x35670010, 2,097,152)` and `(0x39C52040, 2,101,250)` from `sub_009B5090`, again from `sub_00A02A40` | Task 2: checked against win32hle's reservations and remembered |
| D3DX DXT mip levels | `D3DXCreateVolumeTextureFromFileInMemoryEx` (thunk `sub_00B5230C`), a DXT1 128x128 volume | Task 3: our DXT1/3/5 encoder |
| Effect parameter blocks | `sub_00963C80` records them (23 in the traced run), `sub_00965F10` applies them | Task 4 |
| D3DX compression into DXT | `D3DXLoadSurfaceFromSurface` (thunk `sub_00B52360`), A8R8G8B8 512x512 into DXT5, then `D3DXFilterTexture` (8 of each in the traced run) | Task 3 |
| Unresolved indirect call at `0x00AD468C` | the CRT memmove's backward tail (`neg ecx`, table at `0xAD4668`) | Task 1: backward table slots after `neg` |
| `HeapCompact` | `sub_006139E0`, right after the land's init | Task 2 |
| Unresolved indirect call at `0x00746700` | from `ScriptLibraryR.dll`'s `sub_10007E20`, a start named only by the script native table | Task 1: the 7 data-named starts |
| Metal command buffer out of memory | the land load | Task 7 (below) |
| macOS kills the process | the intro and the land | Task 8 (below) |

**`HeapCompact`** (Task 2's reading of the lifted code): `sub_006139E0` calls it at `0x613C6D` through
the IAT slot `0xC12230`, with the heap handle from `[0x240042C]` and flags 0, and drops the result (the
next instruction sets up `sub_00930DD0`, and the function returns TRUE).

**The extent behind `0x746700`** (Ruling 12): discovery's catalogued `sub_007466A7` claims an extent to
`0x746753`, across the int3 padding at `0x7466FB`-`0x7466FF`, so `immediate_starts`' "not covered" rule
never sees `0x746700`. Discovery's extents were not reworked; `data_starts` names the start instead.

**The land load.** After the click the game saves an autosave in the new profile's folder under My
Documents (folders made with `SHCreateDirectoryExA` from `sub_0060FFE0`, then `MoveFileA` from
`sub_006137B0` and `RemoveDirectoryA` from `sub_0060F910`: five files, about 21 MB), calls `HeapCompact`,
and opens the intro.
Nothing is written under `<support>/Game/`. The game's own init timing line (its debug output) put the
land's init at 3.1, 12.2 and 23.8 s in the three runs (2.9, 12.0 and 23.5 s of it in its map script), and
3.3 to 3.6 s in three more runs; the click-to-open time was 5.8, 15.1 and 26.5 s (6.3 s in another run).
The slow runs ran while other programs kept the Mac busy (1-, 5- and 15-minute load averages of 2 to 14 read around them); the
plan's spike measured 23.2 to 23.6 s. Nothing here separates the two causes further; the tests do not
depend on the load time.

**The intro, measured** (three runs): open to last frame 74.9 s every time (the movie's 1,798 frames at
24 fps). The brightest frame during the movie was 75.26% non-black in every run (the 640x360 picture
scaled into 1024x768 with black bars), so the campaign test's movie bound is 3/8. The last frame is
copied, Goto is asked for 1,799 once, and then the land's opening scene is drawn in 3D: frames 501 to 600
after the last movie frame were each at least 99.97% non-black in every run, and no frame after the intro
was under half non-black in up to 600 s.

## The Metal findings (Tasks 7 and 8)

**Task 7 Step 1.** During the land load (no Present for the whole map load) one command buffer stayed
open 16 to 19 s with 37,159 uploads, 37,446 blits, one render pass and 2 draws, and staged 38,679,035,923
bytes for 504,444,945 bytes of source span: `mtl_tex_upload` sized each staging buffer as the whole level
(slice pitch times depth), not the box. 36,864 of the uploads were 16x16 boxes into 1024x1024 levels
(`sub_00A532B0`; 28,672 BC2, 8,192 R8). Live D3D9 surfaces stayed at 4 or 5 and textures at 459, so no
object leaked; the GPU's allocated size passed 40 GB on the 32 GB Mac, and the buffer failed with
Insufficient Memory (once with a GPU hang). After the load each frame staged 0.9 to 1.4 GB the same way
(2x2 to 4x4 boxes into a 1024x1024 RG8 texture, `sub_00938210`). **Fix:** staging holds only the box,
its rows packed, and a command buffer is sent once its staging passes 64 MB (Ruling 13).

**Task 8.** With that fixed, the campaign run was killed by macOS (SIGKILL, no crash report, no message
in the engine's output) 327 and 470 s after the start. Sampling the process's footprint every 10 s showed
it grow by about 1.1 GB a second while the intro played (80 GB at the intro's end, 129 GB at 313 s, nearly
all of it IOAccelerator memory), and by about 0.2 GB a second on the land. The cause: Metal hands out
command buffers, encoders, descriptors and drawables autoreleased, a command buffer keeps everything its
commands used (the frame's staging buffers) until it is freed, and the guest thread has no run loop and
never leaves an autorelease pool, so nothing was ever freed. The movie path (a 2 MB lock of the movie's
1024x512 level every frame) made it fast. **Fix:** `mtl_commit` (every Present, readback and 64 MB send
goes through it) pops the autorelease pool the previous commit on that thread pushed and pushes a new
one. The selftest group `movie_frames` (1,500 frames of the movie path) grew the footprint by 3,003 MB
before the fix and stays within 256 MB after it. With the fix the footprint stays near 1.0 to 1.2 GB for
the whole 600 s campaign run.

**`GetSurfaceLevel`** (plan "Measured" item 3, not re-measured): the game asks for the movie texture's
level surface every frame and releases it, and ours does not outlive that `Release`, so each frame gets
a new surface object. Windows hands back the same object while the texture lives. Nothing measured
depends on it; it is noted for a later D3D9 plan.

## FFmpeg

`out/bw2` links Homebrew's FFmpeg 9.0.2 (`/opt/homebrew/opt/ffmpeg`), as `test_engine_ffmpeg_links`
records:

| Install name | Compatibility | Current |
| --- | --- | --- |
| `/opt/homebrew/opt/ffmpeg/lib/libavformat.63.dylib` | 63.0.0 | 63.1.102 |
| `/opt/homebrew/opt/ffmpeg/lib/libavcodec.63.dylib` | 63.0.0 | 63.1.102 |
| `/opt/homebrew/opt/ffmpeg/lib/libavutil.61.dylib` | 61.0.0 | 61.1.102 |

That build is configured with `--enable-gpl --enable-version3`. It is fine for a local engine and never
leaves this Mac: `out/bw2` and anything linked against it are never copied elsewhere, bundled or
published. The app the Builder makes (a later plan) bundles a minimal LGPL-only build of the pinned
`ffmpeg-9.0.2.tar.xz` instead (Ruling 2).

## The repeated folder

One texture name the game asks for during the land load repeats the water art folder nine times before
its file part (plan "Measured" item 7). It is the game's own, and Windows builds the same name:

- `sub_00AC7950(index, name)` formats the bump-map file name from `name` with the format at `0xD10238`
  (the water art folder, the name, a "High" suffix and the `.dds` extension), tries to load it, and falls
  back to the default high bump map;
- its caller here is `sub_00AC82D0`, which reads a chunk (tag at `0xD101F4`, at most `0x44C` bytes)
  from the land's terrain file with `sub_00920070`, `sub_0091FE60` and `sub_0091FF50`, and passes the
  chunk's first bytes as `name` (logged at the call: indices 0 and 1, the same name);
- that chunk in `Data/landscape/BW2/Land1.ter` (at offset `0x112DD4F`, after its size `0x44C`) begins with
  a 128-character name that already holds the folder eight times, in capitals. All eleven shipped
  `.ter` files hold it.

No answer of ours takes part: the name reaches `GetFullPathNameA` (`sub_00AD7FAA`, the CRT's full-path
call) already repeated, and that answer and the file open are as for any other missing file. The file
does not exist, and the game loads its default bump map.

## Frame rates (headless)

Presents per second, which the test's own frame log counts (the movie itself shows 24 or 25 frames per
second whatever the rate):

| Run | During the movie | Land, first 30 s | Land, later |
| --- | --- | --- | --- |
| attract, fresh | 69 and 41 | - | - |
| attract, reused folder | 116 | - | - |
| campaign, fresh, idle Mac | 343 | 56 | 75 to 94 (11 to 13 after 510 s) |
| campaign, fresh, busy Mac | 143 | 7 to 23 | 20 to 50 |
| campaign, reused folder, busy Mac | 61 | 9 | 5 to 11 |

With a reused support folder the profile already exists, so the front end shows no profile dialog and
compiles its effect before the first input read; the boot tests always start from a fresh folder.

## What was absent

Over the whole traced campaign run (the boot, the profile, the click, the land load, the intro and 600
frames of the land): no `GetVolumeInformation*`, `GetDriveType*`, `GetLogicalDrive*`, `QueryDosDevice*`,
`DeviceIoControl`, service-manager or SetupAPI call, no device path opened, and no tripwire. The
protection VM's code (`stxt774`, `.name0`) called only `GetCurrentThreadId`, as in every earlier boot
(`boot.md`). No `[module]` line beyond those the boot printed before plan 9.

## For the next plan

The campaign boot test now ends 600 Presents after the intro's last frame, on the first land's opening
scene, drawn in 3D with no stop (exit 0 through `BW2_QUIT_AFTER_MOVIE=1798:1798+600`). The game runs on
past that with no stop for at least 600 s. In the land it reads the mouse once per frame
(`IDirectInputDevice8A::GetDeviceData`, 600 reads in 600 frames) and pumps its messages; the scripted
input ends at the click, so whatever the scene waits for from the player (the plan's guess: the creature
choice) is the next plan's to drive.

Plan 11 drove it on: Escape skips the intro and both of the land's scenes, the choice is live after 12 or
13 held presses (the first skips the intro), and it waits there for the player to pick one of four creatures with the game's hand. A
save is taken over the live choice from the pause bar and loaded back into it, and the settings, the save,
the load and where each boot test now ends are in `saves.md` (its "For the next plan").

Imports first seen after the intro (the traced run), all answered: `ID3DXEffect::SetFloat`,
`ID3DXEffect::SetBool`, `ID3DXEffect::SetVectorArray`, `D3DXVec4Transform`, `D3DXVec3Transform`,
`D3DXVec4Normalize`, `D3DXMatrixTranslation`, `D3DXMatrixRotationZ`, `D3DXMatrixScaling`,
`D3DXCompileShader` (3 calls, through the lifted D3DX), and the device's `GetRenderState`,
`GetVertexShader`, `SetClipPlane`, `StretchRect`, `GetSamplerState`, `GetTextureStageState` and
`GetTransform`. Imports first seen between the click and the intro include `VirtualLock`, `HeapCompact`,
`D3DXAssembleShader`, `D3DXCreateSphere`, `D3DXCreateCubeTexture`, `D3DXFillCubeTexture`,
`D3DXLoadSurfaceFromMemory`, `IDirect3DTexture9::AddDirtyRect`, the parameter-block methods and the
autosave's file calls.
