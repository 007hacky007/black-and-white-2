# The boot through the profile dialog (plan 6)

Measured 2026-11-12 on the machine of `frontend.md`, with `out/bw2` relinked by
`python -m bw2.pipeline --build-only` (114,837,112 bytes), on the patched tree, headless, with a
fresh support folder each run:

```
BW2_HEADLESS=1 BW2_FRAME_LOG=1 BW2_INPUT_SCRIPT=<file> BW2_SUPPORT=<folder> \
    out/bw2 --game gamefiles/game
```

The script is `PROFILE_SCRIPT` in `tests/test_boot.py` (coordinates and key codes only). The game
boots to its front end as in `frontend.md`, takes the scripted clicks and keys in its first dialog,
writes a new profile named `ab`, applies the profile's display settings (a device `Reset`) and stops
at its first effect given as HLSL text:
`[bw2] reached D3DXCreateEffect (HLSL source): owned by a later plan`, exit 4. That layer is plan
9's (the HLSL compiler).

Nothing else stops the run: no `TRIPWIRE`, no trap, no unresolved call, no unimplemented import or
method, no `FATAL`, no uncaught C++ exception, and no other later plan's stop. The bring-up loop of
Task 7 found no stop to fix: the boot test passed on its first run after Task 6 (its first version
matched the stop line exactly, and the line carries the guest address after the plan text, so the
test matches its start). The scripted read numbers from "Measured" items 6-9 hit on the first try,
so nothing was re-measured with frames.

`tests/test_boot.py::test_boot_profile_to_hlsl` pins this:
- exit code 4 and exactly one line starting with the HLSL stop above;
- no loud line before the stop, and no other `owned by a later plan` line;
- `Profiles/~ab/Profile.ini` and `Settings.ini` under
  `<support>/C/Documents and Settings/Player/My Documents/Black & White 2/`, both non-empty; no
  `~Player` folder; no file under `<support>/Game/`;
- the DirectInput first-read marker exactly once, and at least one frame after it, every one of
  them at least half non-black (in every run all of them are 786,432 of 786,432).

The arguments below come from a throwaway log in win32hle's shim entry (`host_lite.c`), which
printed each selected import's stack arguments, a few structures behind them, and its answer. It
was reverted before anything was committed. The counts come from a run with
`BW2_TRACE_IMPORTS=1`.

## Timing

Untraced runs, each with a fresh support folder. "First read" is the DirectInput first-read
marker; "stop" is the HLSL stop line; the frame is the last frame line printed before each.

| Run | First read | Its frame | Stop | Its frame | Exit |
|---|---|---|---|---|---|
| 1 (first after the relink) | 5.704 s | 584 | 9.228 s | 663 | 4 at 9.401 s |
| 2 | 3.113 s | 500 | 6.412 s | 579 | 4 at 6.541 s |
| 3 | 2.772 s | 647 | 6.179 s | 726 | 4 at 6.235 s |

In every run the stop comes 79 frames after the first read, 3.3 to 3.5 s later. The frame count
at the first read varies with loading speed, as in `frontend.md`; the read numbers do not.

## DirectInput

The calls, from the traced run, with the arguments as the game passes them (unchanged from plan 5
and "Measured" items 1-4):

| Call | From | Arguments | Answer |
|---|---|---|---|
| `DirectInput8Create` | `sub_00AD4340` | hinst `0x400000`, version `0x800`, riid `0xD061C8` | `DI_OK` |
| `IDirectInput8A::CreateDevice` | `sub_009AF220` | `GUID_SysMouse` (`0xD06188`), outer NULL | `DI_OK` |
| `SetDataFormat` | `sub_009AF220` | `0xD118F4`: size 24, object size 16, flags 2, data size 16, 7 objects at `0xAD42D0` | `DI_OK` |
| `SetCooperativeLevel` | `sub_009AF220` | hwnd `0x10010`, 6 (non-exclusive, foreground) | `DI_OK` |
| `SetProperty` | `sub_009AF220` | `DIPROP_BUFFERSIZE` (1), header {20, 16, 0, `DIPH_DEVICE`}, 512 | `DI_OK` |
| `Acquire` | `sub_009AF220` | - | `DI_OK` |
| `Unacquire` | `sub_009AF790` | - | `DI_OK` |
| `SetCooperativeLevel` | `sub_009AF790` | hwnd `0x10010`, 5 (exclusive, foreground) | `DI_OK` |
| `Acquire` | `sub_009AF790` | - | `DI_OK` |
| `GetDeviceData` | `sub_009AFD20` | element size 20, n = 512 on entry, flags 0 | `DI_OK` (80 of 80) |

The seven objects are `{X, 0, 0x00FFFF03}`, `{Y, 4, 0x00FFFF03}`, `{Z, 8, 0x80FFFF03}` and
`{NULL, 12..15, 0x00FFFF0C or 0x80FFFF0C}`, all with flags 0 (GUIDs at `0xD061B8`, `0xD061A8`,
`0xD06198`), as plan 6's "Measured" item 2 says.

- **Reads per frame:** one. The run makes 80 reads, at frames 430 to 509 of the traced run, one
  per frame, all from `sub_009AFD20`. The 80th read is the last: the game confirms the profile on
  the button's down (read 80), and the profile, the `Reset` and the HLSL stop follow before any
  other read. So the script's last line (`84 up 0`) is never pushed.
- **Events the script produced:** 21 host events pushed (2 moves, 3 button downs and ups at reads
  14, 16 and 80, 16 keys). DirectInput returned 7 elements in 5 reads: 2 at read 10 (X and Y of
  the first move), 1 at read 14 (button 0 down), 1 at read 16 (up), 2 at read 50 (the second
  move) and 1 at read 80 (down). Every other read returned 0 elements. No read returned
  `DI_BUFFEROVERFLOW`, `DIERR_INPUTLOST` or `DIERR_NOTACQUIRED`.
- **The mouse is exclusive from the mode switch on**, so no `WM_MOUSE*` or button message is
  posted: the game takes its clicks from DirectInput alone.

## The name field: IMM32, keys and characters

All on window `0x10010`, in order. The click's down (read 14) gives the field the keyboard:

| Call | From | Arguments | Answer |
|---|---|---|---|
| `ImmAssociateContext` | `sub_00B52282` | `0x10010`, NULL (frame 2, before the front end) | NULL |
| `ImmCreateContext` | `sub_00B522CA` | - | `0x494D0001` |
| `ImmAssociateContext` | `sub_00B52282` | `0x10010`, `0x494D0001` | NULL |
| `GetKeyboardLayout` | `sub_00AF26D0` | 0 | `0x04090409` |
| `ImmGetDescriptionA` | `sub_00B522D0` | `0x04090409`, buffer, `0x100` | 0 |
| `ImmGetProperty` | `sub_00B522C4` | `0x04090409`, `IGP_PROPERTY` (4) | 0 |
| `ImmGetContext` | `sub_00B52288` | `0x10010` | NULL |

When the confirm click is taken (read 80) the field gives the keyboard up: `ImmGetContext(0x10010)`
(NULL) and `ImmAssociateContext(0x10010, NULL)`. The run makes 8 IMM32 calls in all: 3
`ImmAssociateContext`, 2 `ImmGetContext`, and one each of `ImmCreateContext`,
`ImmGetDescriptionA` and `ImmGetProperty`. `ImmDestroyContext`, `ImmGetOpenStatus`, the
composition and candidate calls and `ImmNotifyIME` are not called. This order differs from the
spike's in "Measured" item 7 (which saw two associations with NULL and two `ImmGetContext` before
`ImmCreateContext`); the table is what the engine's answers give.

**Keys.** The 8 presses (6 Backspaces, `A`, `B`, each down and up on its own read) give 24
messages, all through the game's pump: `PeekMessageA` (`sub_009B10D0`), then `GetMessageA`,
`TranslateMessage` and `DispatchMessageA` (all `sub_009B1100`). For each press, in order:
- `WM_KEYDOWN` (vk, lParam `0x00ss0001`, scan `ss` = `0x0E`, `0x1E` or `0x30`), during which the
  window procedure (`sub_009B1430`) calls `GetKeyState(VK_LMENU = 0xA4)` once;
- `WM_CHAR` (8, `0x61` or `0x62`, the same lParam), which reaches `IsDBCSLeadByte` (`sub_00AF1850`)
  once with the character (answered FALSE);
- `WM_KEYUP` (lParam `0xC0ss0001`).

So 8 `GetKeyState` and 8 `IsDBCSLeadByte` calls. `TranslateMessage` and `DispatchMessageA` are
called 26 times in the run: the 24 above and 2 `WM_PAINT` before the front end. `GetMessageA` is
called 26 times, `DefWindowProcA` 39 times (27 after the first read).

**Confirm.** `PathGetCharTypeW` (`sub_00856C00`) is called 18 times, 9 for each character of the
name (`0x61`, `0x62`), each answered `GCT_LFNCHAR | GCT_SHORTCHAR`.

## The profile files and folders

`CreateDirectoryA`, all from `sub_00AD997A`, under
`C:\Documents and Settings\Player\My Documents\` (the support folder's `C/` tree):

| When | Folder | Answer |
|---|---|---|
| before the front end | `Black & White 2` | TRUE |
| before the front end | `Black & White 2\Profiles` | TRUE |
| before the front end | `Black & White 2\Cache` | TRUE |
| at read 80 | `Black & White 2\Profiles\~ab` | TRUE, then FALSE 5 times (it exists) |

The two files, all through `CreateFileA` from `sub_00AEF0C8`:
- before the front end, `Black & White 2\Settings.ini` is opened for reading (`OPEN_EXISTING`) and
  is not there (`INVALID_HANDLE_VALUE`);
- at read 80, `Profiles\~ab\Profile.ini` is opened for reading and is not there either; then the
  pair `Settings.ini` and `Profile.ini` is written (`GENERIC_WRITE`, `CREATE_ALWAYS`) 6 times, with
  `Profile.ini` read back once between each pair and the next (5 reads).

The run leaves `Black & White 2\Settings.ini` (157 bytes) and `Black & White 2\Profiles\~ab\Profile.ini`
(6,201 bytes), the four folders, and an empty `<support>/Game/`.

## The window and the screen size

**Before the first device** (in order; `sub_009A9760` uses a stored window handle that is still 0
here, so its calls on hwnd 0 fail):
1. `SetWindowLongA(0, GWL_STYLE, 0x00C30000)`, `SetWindowLongA(0, GWL_EXSTYLE, 0)`,
   `SetWindowPos(0, HWND_NOTOPMOST, 0, 0, 0, 0, 0x27)` and `(.., 0x03)`, all answered 0;
   `ClipCursor(NULL)` (the first of the four `ClipCursor` calls, all from `sub_009A9760`).
2. `GetSystemMetrics(SM_CMOUSEBUTTONS)` = 3 and `(SM_MOUSEWHEELPRESENT)` = 1 (`sub_009AF550`);
   `GetDoubleClickTime` = 500 (`sub_00C098A0`); `LoadCursorA(0x400000, 0xCE)` and
   `LoadCursorA(NULL, IDC_ARROW)` (`sub_009B1A50`).
3. `GetSystemMetrics(SM_CYSCREEN)` = 1080 and `(SM_CXSCREEN)` = 1920 (`sub_009B1250`), then
   `CreateWindowExA(WS_EX_TOPMOST, .., WS_POPUP, 50, 50, 1920, 1080, ..)` = `0x10010` and
   `ShowWindow(SW_SHOWNORMAL)`. Under win32hle's old default the screen was 640x480; the engine
   now answers the desktop mode (1920x1080 headless), and the window starts at that size.
4. `ShowCursor(FALSE)` = -1 (`sub_00611260`).
5. Twice: `SetMenu(0x10010, NULL)` (`sub_009B10C0`), `SetWindowLongA(0, GWL_STYLE, 0x90800000)`,
   `(0, GWL_EXSTYLE, 8)`, `SetWindowPos(0, HWND_TOPMOST, 0, 0, 0, 0, 0x27)` and `(.., 0x03)`, a
   failed `GetWindowRect(0, ..)`, and `ClipCursor` of the rect it left unfilled: stack contents,
   {11883128, -1, -559087616, 13164524} and then {11883128, -1, -559087616, -559087616}. Right is
   below left in both, so each clears the clip.
6. The DirectInput setup (above).
7. `AdjustWindowRect({0, 0, 1024, 768}, 0x00C30000, FALSE)`, answered unchanged (win32hle windows
   have no frame), `SetWindowPos(0x10010, HWND_NOTOPMOST, 0, 0, 1024, 768, SWP_NOMOVE |
   SWP_NOZORDER)` and `ShowWindow(SW_SHOW)` (all `sub_009A9CC0`), then `GetClientRect` =
   {0, 0, 1024, 768} (`sub_00A25D90`). The window is now 1024x768 at (50, 50).
8. `GetAdapterModeCount` for formats 22, 24, 23 and 35 (13, 0, 13 and 0; `sub_0096DB60`),
   `GetAdapterDisplayMode` = 1920x1080, 60 Hz, `X8R8G8B8` (`sub_00954730` and `sub_00954910`),
   and `IDirect3D9::CreateDevice(0, HAL, 0x10010, 0x40, ..)` (`sub_00954FA0`).

**Just after the device** (`sub_009B10C0` and `sub_009A9760`, now with the real handle):
`SetMenu(0x10010, NULL)`, `SetWindowLongA(GWL_STYLE, 0x90800000)` (was `0x90000000`),
`(GWL_EXSTYLE, 8)`, `SetWindowPos(HWND_TOPMOST, .., 0x27)` and `(.., 0x03)`, `GetWindowRect` =
{50, 50, 1074, 818}, and the fourth `ClipCursor`, of that rect.

**Around the profile `Reset`** (read 80): `GetAdapterIdentifier` (twice), `GetAdapterModeCount(22)`
= 13 and 13 `EnumAdapterModes` (`sub_00955480`), 43 `ID3DXEffect::OnLostDevice`,
`GetAdapterDisplayMode` = 1024x768, 60 Hz, `X8R8G8B8` (`sub_00954910`; the full-screen device's
mode), then from `sub_00955140`: `SetWindowLongA(GWL_STYLE, 0)` (was `0x90800000`),
`SetWindowLongA(GWL_EXSTYLE, 0x10000008)` (was 8), `SetWindowPos(HWND_TOPMOST, 0, 0, 1024, 768,
SWP_FRAMECHANGED)`, `ShowWindow(SW_SHOW)` and `SetForegroundWindow`. Then
`IDirect3DDevice9::Reset` (`sub_009552B0`) with the parameters at the same address as the first
device's: 1024x768, `A8R8G8B8`, 1 back buffer, no multisampling, discard, hwnd `0x10010`, full
screen, auto depth `D24S8`, flags 0, 60 Hz, immediate; answered `D3D_OK`. After it:
`AdjustWindowRect({0, 0, 1024, 768}, 0x00CF0000, FALSE)` (unchanged), `Clear`, 18
`OnResetDevice`, two state blocks, a vertex and an index buffer, `D3DXGetVertexShaderProfile` and
`D3DXGetPixelShaderProfile`. No `ClipCursor` is called around the `Reset`.

**The screen size the game sees:** 1920x1080 before the device (`GetSystemMetrics` and
`GetAdapterDisplayMode`), and 1024x768 from `GetAdapterDisplayMode` while the full-screen device
lives. `GetSystemMetrics` is not called after the device (4 calls in the run, all above).

## Not called

In the whole traced run (302 distinct imports): `GetCursorPos`, `SetCursorPos`, `SetCursor`,
`GetAsyncKeyState`, `IDirectInputDevice8A::GetDeviceState`, `WaitMessage`, `GetFocus`,
`GetActiveWindow` and `GetForegroundWindow` are never called. Nor is any disc, volume or driver
question (`GetDriveTypeA`, `GetVolumeInformationA`, `GetLogicalDrives`, `QueryDosDeviceA`,
`DeviceIoControl`, service calls), any Bink import, or anything that sets off the tripwire. Nothing
from the protection VM appears.

A run of the same script without its key lines (the clicks only) left the dialog waiting: no
profile folder after 300 s, and no stop. So the `~ab` check depends on the typed name.

## Closing

Cmd-Q and the close button push CLOSE, which `input.c` posts as `WM_SYSCOMMAND` `SC_CLOSE` (0x112,
0xF060) to the game window, as Windows gives the close button and Alt+F4. The game's window
procedure (`sub_009B1430`) quits on that (`cmp 0xF060` at `0x9B1793`) and on Alt+F4 (`WM_SYSKEYDOWN`
`VK_F4` with `GetKeyState(0xA4)` down, at `0x9B16E1`); both call `sub_009A8690`, which sets its quit
flag. It does not handle a bare `WM_CLOSE`: that goes to `DefWindowProcA`, which destroys the window,
and the `PostQuitMessage` of its `WM_DESTROY` is lost, since the loop (`sub_009B1100`) ignores
`GetMessage`'s return. The first version of plan 6 posted `WM_CLOSE`, and the game then ran on with
its window gone (to the safety stop at read 600 in the boot test's first run, and to read 2000 when
let).

With the script `20 close` (headless, fresh support folder, `BW2_QUIT_AFTER_READS=600` as a safety
stop): the game makes 21 DirectInput reads, releases its device objects and `IDirect3D9`, prints its
`#END` debug line and calls `ExitProcess(0)` from `sub_00AD77B6`, exit 0, in 4.4 to 5.7 s (3 runs). No
`DestroyWindow` and no `PostQuitMessage`, and no disc, volume or driver call. Pinned by
`tests/test_boot.py::test_boot_close_quits`.

## For plan 9 (HLSL)

- **The stop:** `D3DXCreateEffect` from `sub_00B5237E` (the same wrapper that makes the boot's 34
  fx_2_0 effects), after the profile `Reset`, at read 80. Arguments: the device, a heap source of
  11,199 bytes (not fx_2_0: it does not start with `0xFEFF0901`), `pDefines` NULL, `pInclude`
  NULL, `Flags` 0, `pPool` NULL (the boot's pooled effects pass the pool from
  `D3DXCreateEffectPool`, `sub_00B52384`), `ppEffect`, and a non-NULL `ppCompilationErrors`.
- **Its text:** no file is opened between the last read and the stop, so the text is built in
  memory. Its first 10,748 bytes equal the text block that `Data/effects/ToneMapping.sdv` (15,125
  bytes, opened earlier in the boot by the effect loader) holds at offset 372; the file's header
  gives that block's length, 11,121, at offset 356 and its offset, 372, at offset 364. The last
  451 bytes of the source differ from the file. SHA-256 of the source:
  `d475259ea2b744efe6ef14ab5f0cb36d5eef6d50f48b6d94b1980ec641bd2854` (compared in a scratch
  folder outside the repo, nothing kept).
- **Time and frame from start:** 6.2 to 9.2 s, 79 frames after the first read (the table above).
- Before it, the profile's `Reset` has run, so plan 9 starts from a device that has been reset
  once in full screen.
