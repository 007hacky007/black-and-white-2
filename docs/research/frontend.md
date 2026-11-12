# The boot through audio to the front end (plan 8)

Measured 2026-11-12 on an Apple M1 Pro (macOS 27.0.1), with `out/bw2` relinked by
`python -m bw2.pipeline --build-only` (114,772,040 bytes), on the patched tree, headless:

```
BW2_HEADLESS=1 BW2_AUDIO_LOG=1 BW2_FRAME_LOG=1 BW2_FONT_LOG=1 BW2_QUIT_AFTER_READS=300 \
    BW2_SUPPORT=<folder> out/bw2 --game gamefiles/game
```

The run gets through everything plan 7 reached (`startup.md`), starts audio, reaches the front end
and reads the mouse once per frame. Nothing stops it: at the 300th device read
`BW2_QUIT_AFTER_READS` ends it with exit code 0 and the line
`[bw2] quit after 300 input reads (BW2_QUIT_AFTER_READS)`.

There is no loud line on the way: no `TRIPWIRE`, no trap, no unresolved call, no unimplemented
import or method, no `FATAL`, no uncaught C++ exception and no later plan's stop. The bring-up
loop of Task 5 found no new stop: the boot ran to the front end on the first run after Task 4.

`tests/test_boot.py::test_boot_to_front_end` pins this, run with `BW2_FRAME_LOG=1`,
`BW2_FONT_LOG=1`, `BW2_AUDIO_LOG=1`, `BW2_QUIT_AFTER_READS=300` and a fresh support folder:
- exit code 0 and the quit line for 300;
- no loud line and no `owned by a later plan` line;
- exactly the 49 font lines of `startup.md`, in order, with their sources (5 runs: 49 each);
- the script plugin's lifted-DLL line with 0 imports unanswered;
- exactly one audio buffer line, `audio buffer 1: 2 ch, 44100 Hz, 16 bit, 21504 bytes, flags 0x18000`;
- at least one audio second with non-silent frames;
- the DirectInput notice exactly once;
- at least 300 frame lines numbered from 1, at least one frame after the first read at least half
  non-black, and the last frame at least half non-black (5 runs: every frame after the first read,
  the last one included, is 786,432 of 786,432 non-black, mean colour 186,183,182);
- no file under `<support>/Game/`.

## Timing

Each run is untraced, with the four log switches above. "First read" is the first
`GetDeviceData` (the DirectInput notice line). "First non-silent second" is the audio log's
first second with non-silent frames; the line prints when that second has been mixed.

| Run | Support folder | First read | Frame at the first read | First non-silent second | Its frame | Quit (300 reads) |
|---|---|---|---|---|---|---|
| first after the relink | fresh | 3.735 s | 694 | second 3, printed at 4.493 s | 725 | 11.07 s, frame 993 |
| 2 | old (run 1's) | 2.349 s | 807 | second 3, printed at 3.395 s | 848 | 10.67 s, frame 1,106 |
| 3 | old (run 1's) | 2.409 s | 820 | second 3, printed at 3.415 s | 860 | 9.81 s, frame 1,119 |
| 4 | fresh | 2.470 s | 846 | second 3, printed at 3.399 s | 882 | 9.84 s, frame 1,145 |

- In every run the audio log's seconds 1 and 2 are silent and second 3 is partly non-silent
  (4,085, 14,837, 14,069 and 11,893 of 44,100 frames). So the first non-silent sample is mixed
  about 0.7 s after the first read (run 2: 3.395 - 14,837 / 44,100 = 3.06 s).
- The frame count at the first read varies with loading speed (567 in a fifth, 60 s run, up to
  846), as plan 8's Ruling 12 expected. The read count does not.
- The support folder held no file under `Game/` after any run.

## The audio sequence

A 60 s run with `BW2_TRACE_IMPORTS=1` (fresh support folder, no quit; the first read came at
7.1 s, as tracing slows the game). The counts are of the calls in its first 60 s, each from the
function the trace names:

| Call | Count | From |
|---|---|---|
| `CoCreateInstance` of `CLSID_DirectSound` | 2 | `sub_008B1400`, `sub_008B3630` |
| `IDirectSound::Initialize(NULL)` | 2 | `sub_008B1400`, `sub_008B3630` |
| `IDirectSound::SetCooperativeLevel` | 2 | `sub_008B1400`, `sub_008B3630` |
| `IDirectSound::GetSpeakerConfig` | 1 | `sub_008B14E0` |
| `LoadLibraryA("dsound.dll")`, `GetProcAddress(DllGetClassObject)` | 1 each | `sub_008B9700` |
| `DllGetClassObject` | 1 | `sub_008B9700` |
| `IClassFactory::CreateInstance`, `IClassFactory::Release` | 1 each | `sub_008B9700` |
| `IKsPropertySet::Get`, `IKsPropertySet::Release` | 1 each | `sub_008B9700` |
| `IDirectSound::CreateSoundBuffer` | 1 | `sub_008B3870` |
| `IDirectSoundBuffer::Lock`, `Unlock` | 19,765 each | 1 from `sub_008B3870` (the zero fill), the rest `sub_008B37D0` and `sub_008B3820` |
| `IDirectSoundBuffer::Play` | 1 | `sub_008B3AA0` |
| `IDirectSoundBuffer::GetCurrentPosition` | 4,171 | `sub_008B3710` |
| any other `IDirectSound` or `IDirectSoundBuffer` method | 0 | - |
| any `waveOut*` call | 0 | - |

The third `CoCreateInstance` of the run is DirectDraw's (`sub_00934F30`, plan 4). No DirectSound
object is released, as plan 8's "Measured" item 1 says.

The buffer line in every run is `audio buffer 1: 2 ch, 44100 Hz, 16 bit, 21504 bytes, flags 0x18000`.

The audio log over a 60 s untraced run (fresh support folder): 59 seconds mixed, 1 voice playing
in each. Seconds 1 and 2 are silent and 57 are non-silent. The peaks of the non-silent seconds
range from 241 (second 3, which is non-silent for only 1,397 of its frames) to 14,763. Six
seconds are partly non-silent (fewer than 44,100 non-silent frames). In the traced run, 52 of 58
seconds are non-silent, with peaks of 2,695 to 14,763.

## The output choice and the probe

- The probe (`sub_008B98F0` through `sub_008B9700`) loads `dsound.dll`, takes
  `DllGetClassObject`, makes the `IKsPropertySet` of `CLSID_DirectSoundPrivate`
  `{11AB3EC0-25EC-11D1-A4D8-00C04FC28ACA}` and asks property 7 of `DSPROPSETID_DirectSoundDevice`
  `{84624F82-25EC-11D1-A4D8-00C04FC28ACA}` once. The engine calls the game's callback (`0x8B9670`)
  once, with one render device: type 2 (WDM), DeviceId `{B5D2D6A1-6E0B-4C1F-9A3E-0DA5B0C0A801}`
  and an empty interface string.
- The interface string does not begin with `\\?\`, so `sub_008B9800` gives -1, no device
  registry key is opened, and the probe returns 3.
- `sub_0088B530` (output type 0, the default) takes the result above 0 and makes the DirectSound
  output (`sub_0088A8D0`). That is the output a Windows XP machine with default acceleration
  takes, and it is the one the engine answers: one looping streaming buffer that the host mixer
  plays (plan 8's Rulings 2 and 3).
- The only registry open in the traced run is from `sub_0060B720`, before the probe.

## OpenAL and waveOut, both unused

- **OpenAL:** the loader object at `0x1D67C00` (vtable `0xCFA634`, functions `sub_008B3150` and
  `sub_008B2D90`) is never called. No run asks for `OpenAL32.dll` or `wrap_oal.dll`; both stay
  later-plan stops (exit 4). Only the type-2 output, a settings choice, may use it. No EAX call is
  made.
- **waveOut:** the waveOut output (`sub_008B92E0`, calls at `0x8B9142`-`0x8B958D`) is made only
  when the probe returns 0 or less. The engine's probe answer never gives that, and no run calls
  `waveOutOpen`, which stops loudly (`waveOutOpen: the waveOut output is not answered`).

## Task 1's fixes

- **Read-only `OPEN_ALWAYS` opens in place.** The audio library opens 59 files with
  `CreateFileA` from `sub_008872F0` in every run. Each existing file opens in the game tree, so
  the banks are read and the mixer plays sound. Nothing is created under `<support>/Game/`.
- **`GetKeyNameTextW`** is called 78 times, all before the first read (the trace names
  `sub_0064A8E0` for all of them; the call instructions are at `0x64A86A`, `0x64A961` and
  `0x64A9C8`, plan 8's "Measured" item 9). 38 of them (scan code 0) get an empty name and 0. The
  other 40 get a name of 1 to 9 characters.
- **`GetKeyNameTextA`**, the fallback after an empty W answer, is called 76 times, all with
  lParam `0x02000000` (scan code 0, bit 25), and all answered 0 with an empty string: 38 with a
  32-byte buffer and 38 with a 64-byte buffer. So each empty W answer leads to two A calls. The
  A call sites are `0x64A8AF`, `0x64A99C`, `0x64AA02`, `0x64AA30` and `0x64AA58`. (The counts
  and the arguments come from a throwaway log in win32hle's two functions, not kept.)
- **The start `0x8B00B0`** (a `"boot"` start in `bw2/catalog/white-1.42.extra-starts.json`) is
  reached: in a 2,400-read run under `lldb`, a breakpoint on `sub_008B00B0` was hit 8 times. There
  is no unresolved call at `0x008B00B0`.

## The bad catalog entry `sub_008B00D6`

`sub_008B00D6` (`0x8B00D6`-`0x8B0110`) starts inside the 8-byte instruction at `0x8B00D5` of
`sub_008B00B0` ("Measured" item 7). The exe has no rel32 `call` or `jmp` to `0x8B00D6` and one
4-byte literal of it, in `_PDATA` (`0x2407368`, data). In the same 2,400-read `lldb` run a
breakpoint on `sub_008B00D6` was hit 0 times, so no run dispatches to it. It is left for later
catalog work; it costs only a dead lifted function.

## The front end

- **Frames:** 36-41 frames per second headless after the first read (four runs: 40.9, 36.0,
  40.5, 40.7; the 60 s run: 2,226 frames in 57.1 s, 39). Every frame after the first read is
  fully non-black.
- **Reads:** one `GetDeviceData` per frame, all from `sub_009AFD20` (the traced run: 2,267 reads
  and 2,268 frames in the 60 s after the first read; the untraced runs: 299 frames between the
  1st and the 300th read). No `GetDeviceState` is called.
- **The mode switch** `sub_009AF790` (`Unacquire`, `SetCooperativeLevel`, `Acquire`) runs once,
  just before the first read. The setup (`sub_009AF220`) runs once, early in the boot.
- **The message pump:** one `PeekMessageA` per frame from `sub_009B10D0` (2,268 in the 60 s
  after the first read). `GetMessageA` (`sub_009B1100`) is called twice in the run, both before
  the first read.
- **Imports new after the first read** (in the 60 s after it, not seen before it):
  `D3DXMatrixOrthoOffCenterLH`, `ID3DXFont::DrawTextW`, `ID3DXSprite::Draw`,
  `IDirect3DDevice9::DrawIndexedPrimitive`, `IDirect3DDevice9::DrawPrimitiveUP`,
  `IDirect3DDevice9::SetIndices`, `IDirect3DTexture9::AddRef`, `IDirect3DTexture9::LockRect` and
  `IDirect3DTexture9::UnlockRect`. 269 distinct imports are called before the first read and 76
  in the 60 s after it.
- **Absent** in the whole traced run: `GetCursorPos`, `GetAsyncKeyState`, `GetKeyState`, any
  Bink import, and any disc or volume question (`GetDriveTypeA`, `GetVolumeInformationA`,
  `GetLogicalDrives`, `QueryDosDeviceA`, `DeviceIoControl`). The tripwire never fires, and
  nothing from the protection VM appears.

## The game's debug lines

The game prints 60 debug lines through `OutputDebugStringA` in each run, all before the first
read, and none after it in the traced run's 60 s. None contains any of the boot test's loud
words, so the test's check is unchanged.
- The model loader prints 12 lines naming model and texture files it does not find: 11 from
  `sub_0093CF20` and 1 from `sub_00963AA0`.
- 43 come from the game's log wrapper `sub_009EEC70`, and the rest from `sub_00611700` (2),
  `sub_006128B0` (2) and `sub_00871410` (1).
- The engine prints the game's text as given. One line from `sub_006128B0` has no trailing
  newline, so the next engine line (in each run, a frame line) follows it on the same line. The
  boot test splits each line before every `[bw2] ` prefix, which the engine's lines start with
  and the game's text never contains.

## For plan 6 (input)

- The game now reads its mouse at the front end, once per frame: `GetDeviceData(20, buf, &n, 0)`
  from `sub_009AFD20`, after the mode switch `sub_009AF790`. Plan 5's stub answers no input, so
  the front end shows its menu and waits.
- No other input call is made in 60 s: no `GetDeviceState`, `GetCursorPos`, `GetAsyncKeyState`
  or `GetKeyState`. The keys the game names come from `GetKeyNameTextW` and `GetKeyNameTextA`
  before the front end, as above.
- `BW2_QUIT_AFTER_READS=<n>` ends a run at the nth device read with exit 0; plan 6 keeps it.
