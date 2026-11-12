# Plan 6: Input, the game window and the profile dialog Implementation Plan

**Status: next (rewritten 2026-11-12, after plan 8).** Written 2026-11-11 and deferred twice, because the game read no input before its shaders (plan 5), its loading screen (plan 7) or its audio (plan 8). Plan 8's boot reaches the front end, where the game reads its mouse once per frame and waits (`docs/research/frontend.md`). This rewrite redoes "Measured" from `frontend.md` and from a spike on the front end, rebases Task 2 on plan 5's stub (`runtime/bw2/input/dinput8.c`, keeping `BW2_QUIT_AFTER_READS`), and moves the boot's end from the front end to the first stop after it.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game takes real input. Its DirectInput mouse reports the Mac's motion, buttons and wheel; its keyboard, mouse, focus and IME messages arrive as Windows sends them; the Mac window shows the game windowed or in Mac full screen (Cmd+Ctrl+F), with Cmd-Q and the close button reaching the game as `WM_CLOSE`. A scripted run then creates a profile in the front end's first dialog and boots on to the next layer, the HLSL effect compile (exit 4, plan 9's).

**Architecture:**
- The Mac side (main thread) turns each `NSEvent` into a small C struct. A pure C translator turns that struct into input events in the game's terms: client coordinates in back-buffer pixels, Windows virtual keys and scan codes, wheel units and mouse deltas. The events go on a locked host queue.
- The guest thread drains that queue whenever the game could see input: when its message queue is empty (win32hle's pump hook), at every DirectInput read, and in `GetCursorPos` and `GetAsyncKeyState`. Draining feeds two consumers from the same events: win32hle's message queue (`hle_input_*`) and the buffers of the acquired DirectInput devices.
- DirectInput stays on plan 5's stub: the same guest COM objects on plan 4's `d3d_com`, with real reads in place of the empty ones. Unimplemented methods keep stopping loudly.
- The cursor policy (hidden, detached from the pointer, which image, the clip) is computed in C from the game's calls and the window's state. The Mac side only applies it.
- A test switch, `BW2_INPUT_SCRIPT`, pushes scripted events into the same host queue at given read counts, so a headless boot test drives the front end through the whole input path.

**Tech Stack:** C and Objective-C (clang, arm64, ARC, macOS 13+), AppKit, CoreGraphics (`CGWarpMouseCursorPosition`, `CGAssociateMouseAndMouseCursorPosition`), pcrecomp win32hle (vendored, MIT), Python 3.14 and pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 2, 5.2 Input, 7.4 Input and display, 7.5 AppKit basics, 9). Facts: `docs/research/frontend.md` (where plan 8 stops).

## Measured before writing (2026-11-12)

Main at `48c1576`, `out/bw2` relinked (114,772,040 bytes), headless, fresh support folder, `BW2_QUIT_AFTER_READS=300`: exit 0. The last engine lines are `[bw2] DirectInput mouse: no input until plan 6` (plan 5's notice, at the first read) and the quit line. Nothing else stops the run.

A throwaway spike branch (deleted, nothing kept) gave plan 5's stub a scripted mouse and keyboard, logged the arguments of the calls below, and dumped a few frames to a scratch folder to see where the dialog's controls are (not kept). In order:

1. **Imports and objects.** DINPUT8 gives only `DirectInput8Create`. The exe holds `GUID_SysMouse` (`0xD06188`), `GUID_XAxis`/`YAxis`/`ZAxis` (`0xD061B8`, `0xD061A8`, `0xD06198`) and `IID_IDirectInput8A`, and no keyboard, joystick or force-feedback GUID. DirectInput is the mouse only; the keyboard comes from window messages.
2. **Setup** (`sub_009AF220`, once, early in the boot), as plan 5 answers it:
   - `DirectInput8Create(hinst, 0x800, IID_IDirectInput8A, ..)`, `CreateDevice(GUID_SysMouse)`;
   - `SetDataFormat(0xD118F4)`: dwSize 24, dwObjSize 16, dwFlags 2 (`DIDF_RELAXIS`), dwDataSize 16, 7 objects `{pguid, dwOfs, dwType, dwFlags}`: `{X, 0, 0x00FFFF03, 0}`, `{Y, 4, 0x00FFFF03, 0}`, `{Z, 8, 0x80FFFF03, 0}`, `{NULL, 12, 0x00FFFF0C, 0}`, `{NULL, 13, 0x00FFFF0C, 0}`, `{NULL, 14, 0x80FFFF0C, 0}`, `{NULL, 15, 0x80FFFF0C, 0}`;
   - `SetCooperativeLevel(0x10010, 6)` (foreground, non-exclusive);
   - `SetProperty(DIPROP_BUFFERSIZE, {20, 16, 0, DIPH_DEVICE}, 512)`, then `Acquire`.
3. **Mode switch** (`sub_009AF790`, once, just before the first read): `Unacquire`, `SetCooperativeLevel(0x10010, 5)` (foreground, exclusive), `Acquire`. From then on the mouse is exclusive.
4. **Reads:** one `GetDeviceData(20, buf, &n, 0)` per frame from `sub_009AFD20`, with n = 512. No `GetDeviceState` (its site `0x9B00EA` is not reached), `GetCursorPos`, `SetCursorPos`, `SetCursor` or `GetAsyncKeyState` in any spike run.
5. **The game's own cursor.** The game draws its cursor itself, from the DirectInput deltas: it starts at client (0,0), and a delta of (512, 300) put its tip at (512, 300) in a dumped 1024x768 frame (1:1, no acceleration). It calls `ShowCursor(FALSE)` once (`sub_00611260`). It calls `LoadCursorA` twice (`sub_009B1A50`: resource 0xCE from the exe, then `IDC_ARROW`), and no `SetCursor`.
6. **The front end's first dialog** (fresh support folder): a profile dialog with a name field holding the default name (`Player`, the user's name from plan 3's `GetUserNameW`) and a confirm button. In back-buffer pixels the field is at about (510, 382) and the confirm button at about (422, 427).
7. **A click in the name field** gives it the keyboard. In order: `ImmAssociateContext(0x10010, 0)` twice, `ImmGetContext` twice, `ImmCreateContext` (`sub_00B522CA`; unanswered today: `unimplemented import ImmCreateContext`), `ImmGetProperty(0x04090409, IGP_PROPERTY = 4)`, `ImmGetDescriptionA(0x04090409, buf, 0x100)`, `ImmAssociateContext(0x10010, <the new context>)`. `GetKeyboardLayout` is called once (`sub_00AF26D0`).
8. **Keys.** The pump (`sub_009B10D0`) peeks each frame; when a message is waiting, `sub_009B1100` takes it with `GetMessageA`, `TranslateMessage` and `DispatchMessageA`. The window procedure (`sub_009B1430`) calls `GetKeyState(VK_LMENU = 0xA4)` once per key press (8 calls for 8 presses). Each `WM_CHAR` reaches `IsDBCSLeadByte` (`sub_00AF1850`; unanswered today), once per character. Six Backspaces and `a`, `b` after the click replaced the default name; without the click the characters reached `IsDBCSLeadByte` but the name did not change.
9. **Confirm.** A click on the confirm button (move, then down and up some reads later) creates the profile:
   - `PathGetCharTypeW` (`sub_00856C00`; unanswered today) 9 times per character of the name (54 for `Player`, all letters);
   - `CreateDirectoryA` of `My Documents\Black & White 2`, its `Profiles` and `Cache` folders (before the dialog) and of `Profiles\~<name>` (6 times); files `Black & White 2\Settings.ini` (157 bytes) and `Profiles\~<name>\Profile.ini` (6,201 bytes) under `<support>/C/Documents and Settings/Player/My Documents/`; nothing under `<support>/Game/`;
   - then the profile's display settings are applied: `GetAdapterIdentifier`, `GetAdapterModeCount`, 13 `EnumAdapterModes`, 43 `OnLostDevice` calls, `GetAdapterDisplayMode` (`sub_00954910`), `SetWindowLongA(GWL_STYLE, 0)`, `SetWindowLongA(GWL_EXSTYLE, 0x10000008)`, `SetWindowPos(HWND_TOPMOST, 0, 0, 1024, 768, SWP_FRAMECHANGED)`, `ShowWindow(SW_SHOW)`, `SetForegroundWindow` (all `sub_00955140`), `IDirect3DDevice9::Reset` with the same mode as the first device (1024x768, `A8R8G8B8`, 1 back buffer, discard, hwnd 0x10010, full screen, auto depth `D24S8`, 60 Hz, immediate), `AdjustWindowRect`, `Clear`, 18 `OnResetDevice` calls, new buffers and state blocks, `D3DXGetVertexShaderProfile`, `D3DXGetPixelShaderProfile`;
   - and then `D3DXCreateEffect(dev, src, 11199, NULL, NULL, 0, NULL, ..)` from `sub_00B5237E`, the first effect given as HLSL text, not fx_2_0 bytes: `[bw2] reached D3DXCreateEffect (HLSL source): owned by a later plan`, exit 4. The boot's earlier effects (32, `shaders.md`) are all fx_2_0 bytes. With the stub answers above, that stop came about 7.3 s after start (one run).
10. **Window calls** (`sub_009B1250`, `sub_009A9760`, `sub_009A9CC0`), before the first device:
    - `GetSystemMetrics(SM_CXSCREEN)` and `(SM_CYSCREEN)`, which answer win32hle's default 640x480 (the engine never calls `hle_set_screen_size`), then `CreateWindowExA(WS_EX_TOPMOST, .., WS_POPUP, 50, 50, 640, ..)` and `ShowWindow`;
    - `GetSystemMetrics(SM_CMOUSEBUTTONS)` and `(SM_MOUSEWHEELPRESENT)` from the mouse code (`sub_009AF550`), answered 3 and 1; `GetDoubleClickTime` once (`sub_00C098A0`);
    - `ClipCursor` 4 times, all from `sub_009A9760`: `ClipCursor(NULL)`; then twice a rect `GetWindowRect(0, ..)` failed to fill, so it holds stack garbage (for example left 11883128, right -559087616); then `(50, 50, 1074, 818)`, the window's rect;
    - `SetMenu(NULL)`, `SetWindowLongA(GWL_STYLE, 0x90800000)`, `(GWL_EXSTYLE, 8)` and `SetWindowPos` 0x27 then 0x03, three times; `AdjustWindowRect(.., 0xC30000, ..)` and `SetWindowPos(HWND_NOTOPMOST, 0, 0, 1024, 768, 6)`.
11. **Absent** in every spike run: any disc, volume or driver question, any tripwire, anything from the protection VM, any Bink call, `GetDeviceState`, `GetCursorPos`, `SetCursorPos`, `SetCursor`, `GetAsyncKeyState`, `WM_XBUTTON*` use.

**win32hle today** (`third_party/pcrecomp/runtime/win32hle/user32.c`):
- `hle_input_mouse`, `hle_input_key`, `hle_input_cursor`, `hle_set_pump_hook`, `hle_proc_hook`, `hle_cursor_hook`, `hle_cursor_image`, `hle_cursor_shown` and `hle_set_screen_size` exist;
- no wheel input, no double-click synthesis, no extended-key bit, no repeat bit; the left/right modifier VKs (`0xA0`-`0xA5`) are posted as they are where Windows posts the generic one (`VK_SHIFT` and so on), though the key state records both;
- `ClipCursor` does nothing and answers TRUE;
- `TranslateMessage` maps VKs to characters on the US layout; `GetACP` is 1252;
- IMM32: 7 of the 14 imports are answered (`ImmAssociateContext`, `ImmGetCandidateListA`, `ImmGetCompositionStringA`, `ImmGetContext`, `ImmNotifyIME`, `ImmReleaseContext`, `ImmSetOpenStatus`); unanswered: `ImmCreateContext`, `ImmDestroyContext`, `ImmGetDescriptionA`, `ImmGetOpenStatus`, `ImmGetProperty`, `ImmSetCompositionStringA`, `ImmSetCompositionWindow`.

**The engine today:** `dinput8.c` (plan 5) answers the setup, the mode switch and both reads with no input, and holds `BW2_QUIT_AFTER_READS`. `d3d_com.h` has the kinds `D3DK_DINPUT8` and `D3DK_DIDEVICE`, and `d3d9_methods.c` has both interface tables. `later.c` no longer lists `dinput8.dll`. `make_window` in `mtl_device.m` makes a fixed-size titled window at the back buffer's size with the Metal layer on its content view; `mac_main.m`'s menu has only Quit. Plan 4's present already letterboxes the back buffer into the drawable (`mtl_device.m`, the viewport in `mtl_present`). `d3d_display_size` gives the main display in pixels (1920x1080 headless).

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings, bytes, frames or screenshots, or generated C. Facts only (addresses, sizes, counts, hashes, coordinates). No test or switch writes the game's frames to disk. Run `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data.
- Docs (this plan, `input.md`, the spec) never quote the game's own output: no debug lines and no UI strings. They describe a dialog or a line in neutral words with the address that makes it. Test sources never contain the game's strings; they assert on the engine's own lines and on file names.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The tripwire stays on, and its allow list (`runtime/bw2/guard.c` `g_allowed`) stays empty. Winsock keeps answering "no network".
- Fail loudly, never return 0 silently:
  - An unimplemented DirectInput method stops with `<Interface>::<Method> not implemented` through plan 4's `d3d_fail`, which adds the `[bw2] FATAL: d3d9: ` prefix and the guest address. Tests match the bare text.
  - `CreateDevice` of any GUID but `GUID_SysMouse` stops loudly naming the GUID, as plan 5's stub does.
  - An unknown `SetProperty` property or `SetCooperativeLevel` value stops loudly, naming it.
  - A malformed `BW2_INPUT_SCRIPT` line stops the run naming the file's line number.
- **Later plans' layers** each exit 4 with `[bw2] reached <name>: owned by a later plan`. HLSL (`D3DXCreateEffect` given text, `D3DXCompileShader`, `D3DXCompileShaderFromFileA`), the fragment linker, `D3DXCreateSphere`, Bink and OpenAL stay as they are. This plan's boot ends at the first of them.
- **`bw2.h` does not change** (every lifted shard includes it, so a change rebuilds the whole game). New cross-file functions go in `runtime/bw2/input/input.h` or are declared `extern` where used, as `register.c` does. Headers under `runtime/bw2/input/` and `runtime/bw2/d3d9/` may change; each change costs a 2-minute relink.
- Exit codes are unchanged: 0 normal (including `BW2_QUIT_AFTER_READS`), 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- **Headless:** `BW2_HEADLESS=1` means no NSApplication and no window, as in plan 4. Every test runs headless except the opt-in GUI tests (`BW2_GUI=1`). Headless, the window counts as active from the start, so a foreground device can be acquired.
- **Threads:** the Mac side never touches guest memory or win32hle state. It only pushes to the host queue (`bw2_in_push`, which takes its own mutex, a leaf lock). Draining happens on the guest thread, under the machine.
- **Mapping decisions (spec 7.4; my calls where the spec leaves them):**
  - Left, right and middle buttons as on Windows. Buttons 3 and 4 (`NSEvent.buttonNumber` 3, 4) go to DirectInput buttons 3 and 4 only, with no `WM_XBUTTON*` message.
  - The trackpad's two-finger click arrives from macOS as a right-button event and is used as one. Control+click is not a right click.
  - Wheel: a wheel mouse's line delta gives 120 per line. A trackpad's precise delta gives one notch of 120 per `BW2_SCROLL_POINTS` = 10 points, with the remainder carried. Only whole notches are sent. Momentum events are ignored. The direction is `scrollingDeltaY` as macOS delivers it, so it follows the user's natural-scrolling setting. Horizontal scroll is ignored.
  - Option + left drag = middle button. If the game thinks Alt is down at that press, it first gets Alt up. Option's key events are then held back until Option is released, and that release is dropped. The middle button goes up when that left button goes up, whatever the modifiers are then.
  - Cmd is never sent to the game. A key pressed with Cmd held goes to the menus. Caps Lock is sent as `VK_CAPITAL` down and up on each toggle. Fn and keypad Clear are not sent.
  - Mouse deltas for DirectInput are the event's `deltaX`/`deltaY` in points, divided by the letterbox scale (view points per back-buffer pixel), with fractions carried. The game moves its own cursor 1:1 with them ("Measured" item 5), so it keeps up with the hand on the trackpad.
  - On deactivation, every key and button the game thinks is down gets its up message first. DirectInput foreground devices are lost. The D3D device is not lost: Cmd-Tab does not force the game through `Reset`.
  - `ClipCursor`: the reported coordinates are clamped to the clip rect, and the real pointer is never trapped. While the window is inactive, nothing is clamped. A rect whose right is not above its left, or whose bottom is not below its top, clears the clip, as Windows does; the game passes such a rect ("Measured" item 10).
  - Exclusive DirectInput mouse, while acquired and active: the pointer is hidden and detached (`CGAssociateMouseAndMouseCursorPosition(false)`). No `WM_MOUSE*` messages are sent and the Win32 cursor position does not move, as on Windows XP, where exclusive mode suppresses legacy mouse messages. The game is in this mode from its front end on.
  - Full screen is macOS full screen (`toggleFullScreen:`, its own Space). Windowed is a resizable window whose first size is the back buffer in points, scaled down to fit 90% of the screen's visible frame. Either way the back buffer is letterboxed into the view.
  - A full-screen device starts in Mac full screen; a windowed one starts windowed. Cmd+Ctrl+F (View > Enter Full Screen) switches at any time without telling the game.
  - The game's screen size (`hle_set_screen_size`, `GetSystemMetrics`, `GetAdapterDisplayMode`) is the desktop mode from the start, or the device's back buffer while a full-screen device lives, as a Windows mode change gives.
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES` (path relative to `third_party/pcrecomp`, then what changed and why). No NFSU2Recomp code. DirectInput, IMM32 and shlwapi follow Microsoft's documentation; no Wine or ReactOS code is read into this repo.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Never push, never add a remote, never change git config, never write the user's real name or email anywhere. Work on branch `plan6`.

## Review Focus

- **Focus lost mid-drag** (Cmd-Tab with a button and a key held): the game gets the ups, and DirectInput reports `DIERR_INPUTLOST` once, then `DIERR_NOTACQUIRED`. `Acquire` gives `DIERR_OTHERAPPHASPRIO` until the window is active again, and the game's retry loop then recovers. Nothing stays stuck. (Task 2, group `focus_loss`.)
- **DirectInput buffer overflow** (more events than `DIPROP_BUFFERSIZE` between reads, for example a 1 kHz mouse during a slow load): the oldest are dropped. The next read returns `DI_BUFFEROVERFLOW` (1) once, and sequence numbers keep increasing. (Task 2, group `di_overflow`.)
- **Letterboxed full screen at another aspect** (1024x768 back buffer in a 1512x982 view): the top-left of the picture gives client (0,0), the bottom-right gives (1023,767), and points in the black bars clamp to the nearest edge. (Task 3, group `geom_letterbox`.)
- **Tiny precise trackpad deltas, and momentum:** notches only in whole 120s, the remainder carried, momentum ignored, the sign as delivered. (Task 3, group `scroll_precise`.)
- **Option released before the button in an Option+drag:** the middle button still goes up exactly once, Alt ends up released, and no left button is ever sent. (Task 3, group `option_middle`.)

---

### Task 1: The input queue, Windows messages and IMM32

**Files:**
- Create:
  - `runtime/bw2/input/input.h`: the event struct, queue, drain and geometry API for the whole plan;
  - `runtime/bw2/input/input.c`: queue, drain, the Win32 side, the activation sequence and `bw2_input_register`;
  - `runtime/bw2/input/imm32.c`: the seven unanswered IMM32 calls.
- Modify:
  - `runtime/bw2/input/dinput8.c`: its `bw2_input_register` becomes `bw2_dinput8_register`, called from `input.c`'s;
  - `runtime/bw2/register.c`: the pump hook (`hle_set_pump_hook(bw2_in_drain_hook)`) after the tables are registered;
  - win32hle `third_party/pcrecomp/runtime/win32hle/user32.c` and `win32hle.h`, three separate patch commits, each with a PATCHES line:
    1. `void hle_input_key_ex(int down, uint32_t vk, uint32_t scan, int extended)`: bit 24 for extended keys, and bit 30 on a repeat (a down for a key already down). The message's wParam is the generic `VK_SHIFT`/`VK_CONTROL`/`VK_MENU` for the left and right modifier VKs. The key state still records both the side and the generic key. `hle_input_key` calls it with `extended = 0`.
    2. `void hle_input_wheel(int delta, int screen_x, int screen_y)`: `WM_MOUSEWHEEL` (0x020A) to the focus window, else the active one. wParam = `(delta << 16) | key state`, lParam = screen coordinates.
    3. Double clicks in `hle_input_mouse`: a second down of the same button within `GetDoubleClickTime` (500 ms) and within `SM_CXDOUBLECLK` (4 px) of the first, in a window whose class has `CS_DBLCLKS` (8), posts `WM_xBUTTONDBLCLK` instead. A third press starts over. Also `int hle_client_to_screen(uint32_t hwnd, int *x, int *y)` (0 if not a window), with the same origin as `ClientToScreen`.
  - `runtime/bw2/input/input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Produces (`input.h`):
  - `enum bw2_in_kind { BW2_IN_MOVE, BW2_IN_BUTTON, BW2_IN_WHEEL, BW2_IN_KEY, BW2_IN_ACTIVATE, BW2_IN_CLOSE };`
  - `struct bw2_in_event { enum bw2_in_kind kind; uint32_t time_ms; int x, y; int dx, dy; int button; int down; int wheel; uint32_t vk, scan; int extended; int repeat; };`
    - x and y are client coordinates in back-buffer pixels; dx and dy are DirectInput deltas;
    - button: 0 L, 1 R, 2 M, 3, 4; wheel: a multiple of 120; `down` also carries active/inactive for ACTIVATE;
    - time is `hle_ticks_ms()`'s clock, stamped by `bw2_in_push`.
  - `void bw2_in_push(const struct bw2_in_event *e);`: any thread; a mutex; a growable ring, with no fixed cap.
  - `void bw2_in_drain(void);`: guest thread only. `void bw2_in_drain_hook(void);` is the pump hook (it drains).
  - `void bw2_in_set_window(uint32_t hwnd);`: the game window the Mac window shows; until set, `hle_first_hwnd()`.
  - `int bw2_in_active(void);`: 1 until an inactive ACTIVATE drains.
  - `void bw2_in_set_mouse_sink(void (*fn)(const struct bw2_in_event *e));`: Task 2's DirectInput hook, called for every MOVE, BUTTON and WHEEL drained, and for every ACTIVATE.
  - `extern int (*bw2_in_mouse_exclusive)(void);`: Task 2 sets it. While it returns 1, mouse events make no Win32 messages and leave the Win32 cursor where it is.
  - `void bw2_in_clip(const int *rect_or_null);`: the clip in screen coordinates `{l, t, r, b}`, used by Task 5's `ClipCursor`; an empty or inverted rect clears it.
  - `void bw2_input_register(void);` (registers `bw2_input_shims`, then calls `bw2_dinput8_register()`); `extern const win32hle_shim bw2_input_shims[];` grows in Tasks 5 and 6.
- **Drain, per event:**
  - MOVE: screen coordinates = client plus `hle_client_to_screen`, clamped to the clip when active. Then `hle_input_mouse(WM_MOUSEMOVE, ...)`.
  - BUTTON: `hle_input_mouse` with the button's down or up message, buttons 0-2 only.
  - WHEEL: `hle_input_wheel`. KEY: `hle_input_key_ex`.
  - ACTIVATE active, sent with `hle_send` to the game window in Windows' order: `WM_ACTIVATEAPP` (1, thread id), `WM_ACTIVATE` (`WA_ACTIVE` 1, 0), `WM_SETFOCUS`.
  - ACTIVATE inactive: up messages for every key and button down; `WM_ACTIVATE` (`WA_INACTIVE` 0, 0); `WM_KILLFOCUS`; `WM_ACTIVATEAPP` (0, thread id).
  - CLOSE: `hle_post_message(window, WM_CLOSE, 0, 0)`.
- **Shims** (`bw2_input_shims`): `GetCursorPos` and `GetAsyncKeyState` drain, then call win32hle's (`bw2_hle_shim`). `GetKeyState` does not drain (it reports the state as of the messages already queued, as Windows' does for the messages already retrieved).
- **IMM32, answered as Windows with no IME on the US layout** (`imm32.c`, in `bw2_input_shims`):
  - `ImmCreateContext` returns a new nonzero handle each call; `ImmDestroyContext` of one returns TRUE, of anything else FALSE;
  - `ImmGetOpenStatus` returns FALSE;
  - `ImmGetDescriptionA(hkl, buf, n)` returns 0 and writes nothing;
  - `ImmGetProperty(hkl, index)` returns 0 for every index;
  - `ImmSetCompositionStringA` returns FALSE; `ImmSetCompositionWindow` returns TRUE.

- [ ] **Step 1: Write the failing tests.** Groups in `input_selftest.c`, each a test in `tests/test_input.py` (`test_<group>`, asserting exit 0 and `"<group>: 0 failed"`). A recorder on `hle_proc_hook`, or `hle_msg_pop` on a window made with a recording class, checks the messages. Groups:
  - `queue`: 10,000 events pushed from a second pthread arrive in order with nothing lost; draining an empty queue is a no-op.
  - `mouse_msgs`, a window at screen (100,50) made with `CS_DBLCLKS`:
    - MOVE to client (10,20) gives `WM_MOUSEMOVE` with lParam `(20<<16)|10`, and `GetCursorPos` gives (110,70);
    - L down/up/down within 500 ms and 4 px gives `WM_LBUTTONDOWN`, `WM_LBUTTONUP`, `WM_LBUTTONDBLCLK`;
    - the same without `CS_DBLCLKS` gives a second `WM_LBUTTONDOWN`; a press 600 ms later gives `WM_LBUTTONDOWN`.
  - `wheel_msgs`: WHEEL -240 gives `WM_MOUSEWHEEL` with `HIWORD(wParam) == (uint16_t)-240` and lParam in screen coordinates.
  - `key_msgs`:
    - `VK_LEFT` (0x25, scan 0x4B, extended) down gives `WM_KEYDOWN` with lParam `0x014B0001`; a repeat gives `0x414B0001`; the up gives `0xC14B0001`;
    - left Shift (vk `0xA0`, scan 0x2A) down gives wParam `VK_SHIFT` (0x10), and `GetKeyState(VK_LSHIFT)` and `GetKeyState(VK_SHIFT)` both read down;
    - left Alt (`0xA4`) down reads down through `GetKeyState(0xA4)` (the game's window procedure asks exactly that, "Measured" item 8);
    - Alt+F4 gives `WM_SYSKEYDOWN`; `TranslateMessage` on Shift+`A` gives `WM_CHAR` 'A', and on Backspace (8, scan 0x0E) gives `WM_CHAR` 8.
  - `activate`: with L and `A` held, an inactive ACTIVATE gives `WM_LBUTTONUP`, `WM_KEYUP` 'A', then `WM_ACTIVATE` 0, `WM_KILLFOCUS`, `WM_ACTIVATEAPP` 0, in that order, and `bw2_in_active()` is 0; an active one gives `WM_ACTIVATEAPP` 1, `WM_ACTIVATE` 1, `WM_SETFOCUS`.
  - `close`: CLOSE posts `WM_CLOSE` to the window set by `bw2_in_set_window`.
  - `drain_points`: an event pushed before `GetAsyncKeyState('A')` is seen by it; one pushed before `GetKeyState('A')` is not, until the pump hook runs.
  - `imm`: the game's sequence ("Measured" item 7) through `call()` as lifted code makes it: `ImmCreateContext` twice gives two distinct nonzero handles, `ImmGetProperty(0x04090409, 4)` gives 0, `ImmGetDescriptionA(0x04090409, buf, 0x100)` gives 0 and leaves `buf` unchanged, `ImmAssociateContext(hwnd, ctx)` answers, and each of the other four gives the value above. Each call pops its arguments (the `run()` check).
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_input.py -v`. Expected: FAIL (no `bw2_in_*`, no IMM answers); plan 5's `di_*` tests still pass.
- [ ] **Step 3: Patch win32hle** (patches 1-3 above, one commit each with its PATCHES line: "Add extended and repeat bits to key input", "Add mouse wheel input", "Synthesize double clicks"). After each, `.venv/bin/pytest tests/test_engine.py -q` passes.
- [ ] **Step 4: Implement** `input.c`, `imm32.c`, the rename in `dinput8.c` and the pump hook.
- [ ] **Step 5: Run** `.venv/bin/pytest tests/test_input.py tests/test_engine.py tests/test_d3d9.py -q`. Expected: all pass.
- [ ] **Step 6: Commit.** `git commit -m "Add the input queue and Windows input messages"`

### Task 2: The DirectInput mouse on real input

**Files:**
- Modify:
  - `runtime/bw2/input/dinput8.c` (plan 5's stub): keeps `DirectInput8Create`, `CreateDevice`, the setup methods and `BW2_QUIT_AFTER_READS`; `SetDataFormat` now matches objects; the reads report the drained events; the notice `no input until plan 6` goes;
  - `runtime/bw2/input/input_selftest.c`, `tests/test_input.py` (the `NOTICE` checks of `test_di_create` and `test_di_game_setup` become "no notice");
  - `tests/test_boot.py`: `test_boot_to_front_end` no longer expects the notice; the rest of it stands.

**Interfaces:**
- Consumes: Task 1's `bw2_in_set_mouse_sink`, `bw2_in_mouse_exclusive`, `bw2_in_active`, `bw2_in_drain`. Plan 4's `d3d_new`, `d3d_host`, `d3d_impls`.
- Produces: `void bw2_dinput8_register(void);`. Values (DirectInput 8 headers), beside those `dinput8.c` already defines:
  - `GUID_XAxis`/`YAxis`/`ZAxis` {A36D02E0/E1/E2-C9F3-11CF-BFC7-444553540000}; `GUID_Button` {A36D02F0-C9F3-11CF-BFC7-444553540000};
  - `DIERR_INPUTLOST` 0x8007001E, `DIERR_OTHERAPPHASPRIO` 0x80070005, `DIERR_NOTBUFFERED` 0x80040207, `E_NOTIMPL` 0x80004001, `E_HANDLE` 0x80070006, `DI_BUFFEROVERFLOW` 1;
  - `DISCL_EXCLUSIVE` 1, `NONEXCLUSIVE` 2, `FOREGROUND` 4, `BACKGROUND` 8; `DIPROP_AXISMODE` = (GUID*)2 (`DIPROPAXISMODE_ABS` 0, `REL` 1);
  - `DIDFT_RELAXIS` 1, `ABSAXIS` 2, `PSHBUTTON` 4, `TGLBUTTON` 8; instance `(type >> 8) & 0xFFFF`, `DIDFT_ANYINSTANCE` 0xFFFF00; `DIDFT_OPTIONAL` 0x80000000; `DIGDD_PEEK` 1; `INFINITE` 0xFFFFFFFF.
- **The mouse:** 3 axes (X, Y, wheel Z) and 8 buttons (`DIMOUSESTATE2`'s), all relative unless `DIPROP_AXISMODE` or `DIDF_ABSAXIS` says absolute.
- **What stays as plan 5 made it:** `DirectInput8Create`'s version, riid and outer rules; `CreateDevice` of any other GUID loud; `EnumDevices`, `GetCapabilities` and every method the game never calls loud.
- **`IDirectInputDevice8A`:**
  - `SetDataFormat`: dwSize 24, dwObjSize 16, dwDataSize a multiple of 4 with every offset inside it; each object is matched to an axis or button by GUID (or NULL) and type and instance, in order, each device object used once; an object with no match gives `DIERR_INVALIDPARAM` unless `DIDFT_OPTIONAL` (the game's two optional buttons match buttons 2 and 3).
  - `SetDataFormat`, `SetCooperativeLevel` and `SetProperty` while acquired give `DIERR_ACQUIRED`.
  - `SetCooperativeLevel` needs one of exclusive/non-exclusive and one of foreground/background (other bits loud). Mouse exclusive+background gives `E_NOTIMPL`. An hwnd that is not a top-level window gives `E_HANDLE`.
  - `SetProperty`: `DIPROP_BUFFERSIZE` and `DIPROP_AXISMODE` for `DIPH_DEVICE`; anything else loud.
  - `Acquire`: no format gives `DIERR_INVALIDPARAM`; foreground while inactive gives `DIERR_OTHERAPPHASPRIO`; already acquired gives `DI_NOEFFECT`; success empties the buffer and the state.
  - `Unacquire` when not acquired: `DI_NOEFFECT`.
  - Reads (`BW2_QUIT_AFTER_READS` counts first, as now; then a drain): not acquired gives `DIERR_NOTACQUIRED`; after a loss, `DIERR_INPUTLOST` once, and the device is then unacquired.
  - `GetDeviceState(cb, data)`: cb must equal dwDataSize, else `DIERR_INVALIDPARAM`; relative axes report the motion since the last `GetDeviceState`, absolute ones the running sum; buttons read 0x80 when down.
  - `GetDeviceData(cb, rgdod, &n, flags)`: cb 16 or 20, else `DIERR_INVALIDPARAM`; no buffer size gives `DIERR_NOTBUFFERED`; each element is `{dwOfs, dwData, dwTimeStamp, dwSequence[, uAppData = 0]}` at the format's offsets; axis data is the delta (or the absolute value); button data is 0x80 or 0; X and Y from one event share a sequence number, and sequence numbers are global and increase; rgdod NULL with n `INFINITE` flushes and gives the count; `DIGDD_PEEK` keeps the data; overflow drops the oldest and gives `DI_BUFFEROVERFLOW` on the next read.
  - The sink records events only while the device is acquired. A foreground device loses acquisition on an inactive ACTIVATE.
  - `bw2_in_mouse_exclusive` returns 1 while any exclusive mouse is acquired and the window is active.

- [ ] **Step 1: Write the failing tests** (groups and tests in `tests/test_input.py`):
  - `di_format`: the game's format ("Measured" item 2, built in guest memory with the three axis GUIDs) gives `DI_OK`; a format whose object asks for a fourth axis gives `DIERR_INVALIDPARAM`, and the same object with `DIDFT_OPTIONAL` gives `DI_OK`; an offset past dwDataSize gives `DIERR_INVALIDPARAM`.
  - `di_data`, after the game's setup and mode switch:
    - a MOVE (dx 3, dy -2), a BUTTON 0 down and a WHEEL -120 give `GetDeviceData(20, buf, &n = 512, 0)` = 4 elements, `DI_OK`;
    - X (ofs 0, 3) and Y (ofs 4, -2) share a sequence number; button (ofs 12, 0x80); Z (ofs 8, -120); uAppData 0;
    - the timestamps are the push times; the next read gives n = 0.
  - `di_state`: two MOVEs (3, 4) give `GetDeviceState(16)` X 7; a second read with no motion gives X 0; a button held reads 0x80.
  - `di_overflow`: buffer size 4 and 6 button events give 4 elements (the newest 4) and `DI_BUFFEROVERFLOW`; the next read gives `DI_OK`; sequence numbers keep increasing.
  - `focus_loss`: after an inactive ACTIVATE, reads give `DIERR_INPUTLOST`, then `DIERR_NOTACQUIRED`; `Acquire` gives `DIERR_OTHERAPPHASPRIO`; after an active ACTIVATE, `Acquire` gives `DI_OK`; the game's retry loop played back (read, on `INPUTLOST`/`NOTACQUIRED` `Acquire`, read again) ends with no error.
  - `di_exclusive`: coop 5 acquired: a MOVE makes no `WM_MOUSEMOVE` and `GetCursorPos` stays put, but DirectInput gets it; coop 6 (the setup's): a MOVE makes both; after `Unacquire`, a MOVE makes a message again.
  - `di_loud` keeps its five cases; `di_quit` keeps its cases (a scripted read still counts).
  - `test_di_create` and `test_di_game_setup`: no `no input until plan 6` line.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_input.py -v`. Expected: the new groups FAIL; `di_loud` and `di_quit` pass.
- [ ] **Step 3: Implement** the rebased `dinput8.c`.
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py tests/test_d3d9.py tests/test_engine.py -q`, then `.venv/bin/python -m bw2.pipeline --build-only && BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: all pass (the front end still waits, with no input pushed).
- [ ] **Step 5: Commit.** `git commit -m "Feed the DirectInput mouse from the input queue"`

### Task 3: Mac events in the game's terms

**Files:**
- Create: `runtime/bw2/input/translate.c` (pure C, no AppKit).
- Modify: `input.h`, `input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Produces:
  - `struct bw2_mac_ev { int type; double x, y; double dx, dy; int button; unsigned keycode; uint64_t mods; int precise; int momentum; int repeat; double scroll_dy; };`
    - type is one of `BW2_MAC_DOWN`, `UP`, `MOVE`, `SCROLL`, `KEYDOWN`, `KEYUP`, `FLAGS`;
    - x and y are view points with a bottom-left origin (AppKit's);
    - mods are `NSEventModifierFlags` bits: Shift 1<<17, Control 1<<18, Option 1<<19, Command 1<<20, CapsLock 1<<16.
  - `struct bw2_geom { double view_w, view_h; uint32_t bb_w, bb_h; };`
  - `int bw2_translate(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m, struct bw2_in_event *out, int max);`: returns the number of events written (0..4). `struct bw2_translator` holds the carried state: the scroll remainder, the delta fractions, the Option-middle state, the held-back Option, Alt as the game sees it, and Caps Lock.
  - `int bw2_keymap(unsigned mac_keycode, uint32_t *vk, uint32_t *scan, int *extended);`: 0 for keys that are not sent.
  - `void bw2_letterbox(const struct bw2_geom *g, double *x0, double *y0, double *scale);`: the picture's rect in view points. The scale is min(view_w/bb_w, view_h/bb_h), and the picture is centred, as plan 4's present draws it.
- Client coordinates: `cx = floor((x - x0) / scale)`, `cy = floor((view_h - y - y0) / scale)`, each clamped to `[0, bb - 1]`.
- **Key map (US layout; Mac `kVK_*` code to VK, set-1 scan code, extended):**
  - Letters `A`-`Z` to VK 'A'-'Z', with scans Q10 W11 E12 R13 T14 Y15 U16 I17 O18 P19 A1E S1F D20 F21 G22 H23 J24 K25 L26 Z2C X2D C2E V2F B30 N31 M32. Mac codes: A00 S01 D02 F03 H04 G05 Z06 X07 C08 V09 B0B Q0C W0D E0E R0F Y10 T11 O1F U20 I22 P23 L25 J26 K28 N2D M2E.
  - Digits (Mac 1 12, 2 13, 3 14, 4 15, 5 17, 6 16, 7 1A, 8 1C, 9 19, 0 1D) to VK '0'-'9', with scans 1 02 ... 9 0A, 0 0B.
  - Punctuation:

    | Key | Mac | VK | Scan |
    |---|---|---|---|
    | `-` | 1B | BD | 0C |
    | `=` | 18 | BB | 0D |
    | `[` | 21 | DB | 1A |
    | `]` | 1E | DD | 1B |
    | `;` | 29 | BA | 27 |
    | `'` | 27 | DE | 28 |
    | `` ` `` | 32 | C0 | 29 |
    | `\` | 2A | DC | 2B |
    | `,` | 2B | BC | 33 |
    | `.` | 2F | BE | 34 |
    | `/` | 2C | BF | 35 |
    | ISO section | 0A | E2 | 56 |

  - Return 24 to 0D/1C, Tab 30 to 09/0F, Space 31 to 20/39, Delete 33 to 08/0E, Escape 35 to 1B/01.
  - Modifiers: Shift 38 to A0/2A; right Shift 3C to A1/36; Control 3B to A2/1D; right Control 3E to A3/1D extended; Option 3A to A4/38; right Option 3D to A5/38 extended; Caps Lock 39 to 14/3A.
  - Function keys: F1 7A, F2 78, F3 63, F4 76, F5 60, F6 61, F7 62, F8 64, F9 65, F10 6D to VK 70-79, scans 3B-44; F11 67 to 7A/57, F12 6F to 7B/58; F13 69, F14 6B, F15 71 to 7C-7E, scans 64-66.
  - Keypad: 0-9 (52 53 54 55 56 57 58 59 5B 5C) to VK 60-69, scans 52 4F 50 51 4B 4C 4D 47 48 49; `.` 41 to 6E/53, `*` 43 to 6A/37, `+` 45 to 6B/4E, `-` 4E to 6D/4A; `/` 4B to 6F/35 extended, Enter 4C to 0D/1C extended.
  - Navigation, all extended: Home 73 to 24/47, End 77 to 23/4F, Page Up 74 to 21/49, Page Down 79 to 22/51; Forward Delete 75 to 2E/53, Help 72 to Insert 2D/52; Left 7B to 25/4B, Right 7C to 27/4D, Down 7D to 28/50, Up 7E to 26/48.
  - Not sent: Command 37/36, Function 3F, keypad Clear 47, keypad `=` 51, F16-F20.
- **Event rules:**
  - Mouse: DOWN or UP of buttons 0-4 gives BUTTON, plus a MOVE first if the point moved; MOVE gives MOVE (client coordinates, and the delta per the Global Constraints); SCROLL per the wheel rule.
  - Keys: KEYDOWN with Command gives nothing; KEYDOWN and KEYUP give KEY through the key map, with `repeat` carried; FLAGS gives KEY down or up for the modifier whose bit changed; Caps Lock gives down then up.
  - Option + left DOWN is middle, per the Global Constraints.

- [ ] **Step 1: Write the failing tests** (groups and tests in `tests/test_input.py`):
  - `keymap`: every row of the map above, plus the not-sent keys giving 0.
  - `geom_letterbox`, with a 1512x982 view and a 1024x768 back buffer (scale 1.27864..., bars left and right): the picture's top-left point gives (0,0) and its bottom-right gives (1023,767); (0, 490) in the left bar gives (0, 384); a 1024x768 view gives client = point with y flipped.
  - `deltas`: deltaX 1.0 at scale 2.0 gives dx 0, then 1 on the second event (fractions carried); the sign of deltaY follows AppKit's (down positive in both).
  - `scroll_precise`: precise deltas 3, 3, 3 give nothing, then a 4th (total 12) gives WHEEL +120 with remainder 2; momentum events give nothing; -25 gives -240 with remainder -3 carried; a non-precise 1.0 gives +120.
  - `option_middle`: Option down (game sees Alt down), left DOWN, Option up, left UP gives KEY Alt down, KEY Alt up, BUTTON 2 down, BUTTON 2 up; the Option up is dropped, and no BUTTON 0 appears; a later plain left DOWN gives BUTTON 0.
  - `cmd_keys`: Cmd+Q KEYDOWN gives nothing; a Command FLAGS change gives nothing.
  - `caps`: the Caps Lock FLAGS bit set gives `VK_CAPITAL` down and up; cleared gives down and up again.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement** `translate.c`.
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py -q`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Translate Mac events to game input"`

### Task 4: The game's window on the Mac

**Files:**
- Create: `runtime/bw2/d3d9/mac_window.m`: `BW2View` (the layer-hosting content view), the window delegate and the app delegate.
- Modify:
  - `runtime/bw2/d3d9/mtl_device.m`: `make_window` uses `BW2View` through `bw2_mac_window_open`, and `mtl_ctx_resize` calls `bw2_mac_window_mode`;
  - `runtime/bw2/d3d9/mac_main.m`: the menus (app menu: Hide Cmd-H, Quit Cmd-Q; View: Enter Full Screen, `toggleFullScreen:`, Cmd+Ctrl+F; Window: Minimize Cmd-M) and the app delegate;
  - `runtime/bw2/d3d9/d3d9_adapter.c`, `d3d9_adapter.h`, `d3d9_device.c`: the current display mode;
  - `runtime/bw2/main.c`: `--selftest-input-window`;
  - `input.h`, `input_selftest.c`, `d3d9_selftest.c`, `tests/test_input.py`, `tests/test_d3d9.py`, `tests/test_engine.py`.

**Interfaces:**
- Consumes: Task 3's `bw2_translate`, `bw2_geom`; Task 1's `bw2_in_push`, `bw2_in_set_window`.
- Produces:
  - `void bw2_mac_window_open(struct mtl_ctx *c, uint32_t hwnd, uint32_t bb_w, uint32_t bb_h, int fullscreen);`
  - `void bw2_mac_window_mode(struct mtl_ctx *c, uint32_t bb_w, uint32_t bb_h, int fullscreen);`: on `Reset`.
  - `void d3d_current_mode(uint32_t *w, uint32_t *h);` (`d3d9_adapter.h`): `d3d_display_size`, or the full-screen device's back buffer while one lives. `GetAdapterDisplayMode` and plan 4's mode checks read it. `d3d9_adapter_register` calls `hle_set_screen_size` with it at start, and `CreateDevice`, `Reset` and the device's last `Release` update it and call `hle_set_screen_size`.
  - `void bw2_window_fit(double vis_w, double vis_h, uint32_t bb_w, uint32_t bb_h, double *w, double *h);` (`input.h`, pure): the first windowed size.
- **BW2View:**
  - It accepts first responder and first mouse. It has a tracking area for `mouseMoved` and `cursorUpdate:`, and the window accepts mouse-moved events.
  - Each mouse, drag, scroll, key and flags event becomes a `bw2_mac_ev`, goes through `bw2_translate` with the view's current geometry (bounds in points, the back buffer size), then to `bw2_in_push`.
  - `keyDown` never calls `interpretKeyEvents:`, so no press-and-hold accent popup appears.
  - The drawable size follows the view's bounds times the backing scale, on every resize and backing change. Plan 4's present letterboxes into it.
- **Delegates:**
  - `windowShouldClose:` pushes CLOSE and returns NO.
  - Becoming key pushes ACTIVATE 1; resigning key pushes ACTIVATE 0.
  - `applicationShouldTerminate:`, while a game window exists, pushes CLOSE and returns `NSTerminateCancel` (spec 7.5). With none, it returns `NSTerminateNow`.
- The window is resizable, with collection behaviour `NSWindowCollectionBehaviorFullScreenPrimary`. Its size and first state follow the Global Constraints.

- [ ] **Step 1: Write the failing tests:**
  - d3d9 group `display_mode` (`tests/test_d3d9.py::test_display_mode`), headless:
    - before any device, `GetAdapterDisplayMode` and `GetSystemMetrics(SM_CXSCREEN)`/`(SM_CYSCREEN)` give 1920x1080;
    - after `CreateDevice` with Windowed 0 at 1024x768, both give 1024x768, and a window that covered the screen now covers 1024x768 (win32hle's rule);
    - `Reset` to the same full-screen mode leaves 1024x768 (the game's profile `Reset`, "Measured" item 9); `Reset` to Windowed 1 gives 1920x1080 again;
    - a full-screen device's last `Release` gives 1920x1080.
  - input group `window_size` (`tests/test_input.py::test_window_size`): 1024x768 in 1512x945 gives 1024x768; 1920x1080 in 1512x945 gives 1360.8 x 765.45 (90% of the width is the tighter bound; aspect kept).
  - GUI test `tests/test_engine.py::test_window_input` (opt-in, `BW2_GUI=1`), engine flag `--selftest-input-window`:
    - a windowed 640x480 device on a game window made with `CS_DBLCLKS`;
    - synthetic `NSEvent`s posted to the window: left down and up at the view's centre, the `A` key (keyCode 0), a scroll of 3 lines;
    - it prints the messages recorded by `hle_proc_hook`, then `performClose:`;
    - expected stdout lines: `WM_LBUTTONDOWN 320,240`, `WM_LBUTTONUP 320,240`, `WM_KEYDOWN 41 001E0001`, `WM_MOUSEWHEEL 360`, `WM_CLOSE`, `input window ok`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_d3d9.py::test_display_mode tests/test_input.py::test_window_size -v` and `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py::test_window_input -v`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_d3d9.py tests/test_input.py tests/test_engine.py -q`, `.venv/bin/python -m bw2.pipeline --build-only && BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v` (the new start screen size must leave the boot to the front end unchanged), and `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py -k window -v`. Expected: all pass, including plan 4's `test_window_device_presents`. The GUI tests open a small window for a few seconds; if the session has no window server, record them as not run in the ledger rather than skipping silently.
- [ ] **Step 5: Commit.** `git commit -m "Give the game a Mac window with input and full screen"`

### Task 5: The cursor

**Files:**
- Create: `runtime/bw2/input/cursor.c` (the policy, pure C).
- Modify:
  - `runtime/bw2/d3d9/mac_window.m`: applies the policy;
  - `runtime/bw2/input/input.c`: shims `ClipCursor`, `SetCursorPos`, `ShowCursor` and `SetCursor` in `bw2_input_shims`, each calling win32hle's and then updating the policy;
  - `input.h`, `input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Consumes: win32hle's `hle_cursor_shown`, `hle_current_cursor`, `hle_cursor_image`; Task 1's `bw2_in_clip`, `bw2_in_active`; Task 2's `bw2_in_mouse_exclusive`.
- Produces:
  - `struct bw2_cursor_mode { int hidden; int detached; uint32_t image; int warp; int warp_x, warp_y; };` (image: 0 is the system arrow, else a win32hle cursor handle that has an image; warp: a pending warp, in client coordinates).
  - `void bw2_cursor_policy(struct bw2_cursor_mode *out);`: the guest thread computes it after each cursor shim and each drained ACTIVATE.
  - `void bw2_cursor_publish(void);`: hands the mode to the main thread with `dispatch_async` (a no-op headless).
  - `void bw2_cursor_rgba(const hle_cursor_image_t *img, uint8_t out[32 * 32 * 4]);` (pure).
- **Rules:**
  - hidden = inactive ? 0 : (ShowCursor count < 0, or `SetCursor(NULL)`, or detached).
  - detached = active and `bw2_in_mouse_exclusive()`.
  - `SetCursorPos(x, y)` in screen coordinates becomes a warp to the client point (screen minus the client origin) when active and not detached.
  - `ClipCursor(rect)` calls `bw2_in_clip(rect)`; `ClipCursor(NULL)` and an empty or inverted rect clear it. All return TRUE.
- **The Mac side:**
  - `[NSCursor hide]`/`unhide`, kept balanced with a flag, only while the pointer is inside the view.
  - `CGAssociateMouseAndMouseCursorPosition(!detached)`.
  - A cursor image becomes a 32x32 `NSCursor` from the AND/XOR masks: AND 1, XOR 0 transparent; AND 0, XOR 0 black; AND 0, XOR 1 white; AND 1, XOR 1 (inverted) black.
  - A warp maps the client point to view points through the letterbox, then to global display coordinates, then `CGWarpMouseCursorPosition`. After the warp, it calls `CGAssociateMouseAndMouseCursorPosition(true)` unless detached.
  - On resigning key it always unhides and reattaches.

- [ ] **Step 1: Write the failing tests** (groups and tests in `tests/test_input.py`):
  - `cursor_policy`: `ShowCursor(FALSE)` gives hidden (the game's one call, "Measured" item 5); an inactive ACTIVATE gives not hidden and not detached; coop 5 acquired and active gives detached and hidden; `SetCursor(NULL)` gives hidden; `SetCursor(LoadCursorA(0, IDC_ARROW))` with the count at 0 gives image 0, not hidden.
  - `cursor_warp`: a window at screen (100,50): `SetCursorPos(150, 90)` gives warp (50, 40), and `GetCursorPos` gives (150, 90); while detached, no warp.
  - `clip`: in a window at screen (0,0), `ClipCursor({0,0,640,480})` then a MOVE to client (700, 10) gives `WM_MOUSEMOVE` at (639, 10) and `GetCursorPos` (639, 10); after an inactive ACTIVATE, a MOVE is not clamped; `ClipCursor(NULL)` unclamps; the game's garbage rect `{11883128, -1, -559087616, 13098988}` returns TRUE and unclamps; then the game's `{50, 50, 1074, 818}` clamps a MOVE to screen (2000, 2000) at (1073, 817).
  - `cursor_image`: a 32x32 cursor made by the test (AND/XOR rows) gives the RGBA pixels per the mask rule, through `bw2_cursor_rgba`.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py -q` and `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py -k window -v`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Answer the game's cursor calls on the Mac"`

### Task 6: Scripted input, and the Win32 answers the profile dialog needs

**Files:**
- Create: `runtime/bw2/input/script.c`: `BW2_INPUT_SCRIPT`.
- Modify:
  - `runtime/bw2/input/dinput8.c`: each device read calls `bw2_script_at_read(n)` after `BW2_QUIT_AFTER_READS`'s count and before its drain;
  - `runtime/bw2/shims_k32.c`: `IsDBCSLeadByte`;
  - `runtime/bw2/shell.c`: `PathGetCharTypeW`;
  - `runtime/bw2/shims_selftest.c`, `tests/test_engine.py::test_shims_selftest`, `input.h`, `input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Consumes: Task 1's `bw2_in_push`.
- Produces:
  - `void bw2_script_at_read(unsigned long long n);` (`input.h`): pushes every script event whose read number is at most n and not yet pushed.
  - **`BW2_INPUT_SCRIPT=<file>`** (read once, at the first read; unset, nothing changes). One event per line, `#` starts a comment, blank lines are skipped, numbers in C syntax (`strtol` base 0):
    - `<read> move <dx> <dy>`: a MOVE with DirectInput delta (dx, dy); its client point is a running position that starts at (0,0), where the game's cursor starts ("Measured" item 5), moved by the delta and clamped to the game window's client rect (not clamped while there is no window);
    - `<read> down <button>` and `<read> up <button>`: BUTTON 0-4 at the running position;
    - `<read> wheel <delta>`: WHEEL, a nonzero multiple of 120;
    - `<read> key <vk> <scan> <down|up> [ext]`: KEY;
    - `<read> close`: CLOSE.
  - Read numbers must not decrease. A bad line (unknown verb, a missing or extra field, a decreasing read, a wheel that is not a multiple of 120, a button above 4) stops the run with `[bw2] FATAL: BW2_INPUT_SCRIPT line <k>: <what>` (through `d3d_fail`, so the text is bare), and a file that cannot be opened stops naming it.
  - `IsDBCSLeadByte(b)`: FALSE for every byte (code page 1252, `GetACP`'s answer, has no lead bytes).
  - `PathGetCharTypeW(ch)` (shlwapi's `GCT_*`: `GCT_INVALID` 0, `GCT_LFNCHAR` 1, `GCT_SHORTCHAR` 2, `GCT_WILD` 4, `GCT_SEPARATOR` 8): below 0x20 and `"` `<` `>` `|` give 0; `*` `?` give 4; `\` `/` `:` give 8; space and `;` `,` `+` `=` `[` `]` give 1; every other character, letters, digits, `.` and the non-ASCII range included, gives 3.

- [ ] **Step 1: Write the failing tests:**
  - input group `script` (`tests/test_input.py::test_script`), with a script file the test writes: `3 move 510 382`, `5 down 0`, `6 up 0`, `7 key 8 0x0E down`, `7 key 8 0x0E up`, `9 move -88 45`, `9 wheel -120`. After the game's setup and mode switch, `GetDeviceData` reads 1-9 give: nothing on reads 1-2; X 510 and Y 382 (one sequence number) on read 3; nothing on read 4; button 0 down on read 5, up on read 6; X -88, Y 45 and Z -120 on read 9. On read 7 the window gets `WM_KEYDOWN` 8 and `WM_KEYUP` 8 with scan 0x0E.
  - `test_script_bad` (parametrized, each in a child): `2 jump 1 1` names line 1 and `jump`; `5 move 1 1` then `4 move 1 1` names line 2; `1 wheel 100` names line 1; `1 down 7` names line 1; a missing file names the file. Each exits nonzero with `BW2_INPUT_SCRIPT line` (or the file name) in stderr.
  - shims selftest: `IsDBCSLeadByte` gives 0 for 0x00, 0x41, 0x81, 0xE0 and 0xFF; `PathGetCharTypeW` gives 3 for `a`, `P`, `0`, `.`, `~` and 0xE9, 1 for space and `;`, 4 for `*`, 8 for `\` and `:`, 0 for 0x1F and `|`. Each pops its one argument.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_input.py -k script -v tests/test_engine.py::test_shims_selftest`. Expected: FAIL.
- [ ] **Step 3: Implement** `script.c`, the read hook and the two answers.
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py tests/test_engine.py -q`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Add scripted input and the profile name answers"`

### Task 7: The boot through the profile dialog, measured

**Files:**
- Create: `docs/research/input.md`.
- Modify:
  - `tests/test_boot.py`: a new `test_boot_profile_to_hlsl`, and the module docstring;
  - the spec: section 5.2 Input (what the game reads and how it is answered) and 7.4 (a "Plan 6" note with the mapping as built);
  - `docs/research/frontend.md`: "For plan 6" points to `input.md`.

**Interfaces:**
- Consumes: everything above.
- Produces: the measured record plan 9 (HLSL) starts from.
- **The script** (`PROFILE_SCRIPT`, a constant in `tests/test_boot.py` that the test writes to its `tmp_path`; coordinates and key codes only), from "Measured" items 6-9: `10 move 510 382`, `14 down 0`, `16 up 0`; six Backspaces (`key 8 0x0E down` and `up`) at reads 22-33; `40`/`41` `A` (0x41, 0x1E), `42`/`43` `B` (0x42, 0x30); `50 move -88 45`, `80 down 0`, `84 up 0`.

- [ ] **Step 1: Write the failing test.** `test_boot_profile_to_hlsl` (`BW2_GAMEFILES=1`) runs `out/bw2` headless on the game tree with a fresh `BW2_SUPPORT`, `BW2_INPUT_SCRIPT=<tmp_path>/profile.script`, `BW2_FRAME_LOG=1` and a 180 s timeout, and asserts:
  - exit code 4, and `[bw2] reached D3DXCreateEffect (HLSL source): owned by a later plan` in the engine lines;
  - no line in `LOUD` before that stop, and no other `owned by a later plan` line;
  - `<support>/C/Documents and Settings/Player/My Documents/Black & White 2/Profiles/~ab/Profile.ini` exists and is not empty, and so does `.../Black & White 2/Settings.ini`; no `~Player` profile folder; no file under `<support>/Game/`;
  - at least one frame line after the first read, each frame at least half non-black.
  `test_boot_to_front_end` stays as Task 2 left it (no script: the front end waits).
- [ ] **Step 2: Run it.** `.venv/bin/python -m bw2.pipeline --build-only && BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: PASS if Tasks 1-6 answer the game as measured. Otherwise FAIL at the first call they do not.
- [ ] **Step 3: Bring-up loop.** Run the test's command by hand with `BW2_TRACE_IMPORTS=1`, output in a file, reading its tail.
  - Each loud stop in this plan's layers (input, IMM32, window, cursor, display mode, the profile answers), or in plan 4's device code, is fixed with a selftest check in the owning group first, red then green.
  - If the scripted clicks miss (the profile folder is `~Player`, or there is none), the coordinates or read numbers are re-measured with a throwaway frame dump on a scratch branch (never committed) and the script is fixed; the reason goes in `input.md`.
  - A stop at another later plan's name before the HLSL stop is recorded, and the test's expected end changes to it.
  - Each stop outside every plan's layer and each tripwire stops the run and goes to the user, as does any disc, volume or driver query.
- [ ] **Step 4: Measure and write `docs/research/input.md`** (facts only; no game strings, no frames):
  - the DirectInput calls and their arguments as the game makes them, the reads per frame, and the event counts the script produced;
  - the IMM32, key and character calls the name field makes, with counts and addresses;
  - the window calls before the first device and around the profile `Reset` (styles, rects, `ClipCursor`'s four rects) and the screen size the game sees before and after the device, with the engine's new start size;
  - the profile files and folders written (names and sizes);
  - the HLSL stop: the caller, the source length, the defines, include and pool arguments, and the time and frame from start (three runs);
  - whether any `GetCursorPos`, `SetCursorPos`, `SetCursor`, `GetAsyncKeyState` or `GetDeviceState` is called;
  - for plan 9: the first HLSL effect's arguments and where its text comes from, if the trace shows the file it was read from.
- [ ] **Step 5: Update the docs.** Spec 5.2 Input: the game reads one exclusive DirectInput mouse once per frame and its keyboard through window messages; both are fed from AppKit events through one host queue; IMM32 is answered as Windows with no IME; `BW2_INPUT_SCRIPT` drives a headless boot through the profile dialog to the HLSL stop. Spec 5.2 D3DX: the phasing note says HLSL compilation, the fragment linker and `D3DXCreateSphere` are plan 9's, and that the boot reaches the first HLSL effect after the profile dialog. Spec 7.4: a "Plan 6" note listing the mapping as built (the Global Constraints' list). Then run the full suite: `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan6-suite.txt 2>&1; tail -3 .superpowers/sdd/plan6-suite.txt`. Expected: all pass. Then `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py -k window -v`. Expected: pass (or recorded as not run, per Task 4).
- [ ] **Step 6: Audit and commit.** `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through the profile dialog to HLSL"`

## Rulings (my calls; the spec leaves them to the builder)

1. **Scope:** plan 6 answers input, the window and the cursor, and the three Win32 answers the first dialog needs (`ImmCreateContext` and the other IMM32 calls, `IsDBCSLeadByte`, `PathGetCharTypeW`). Its boot ends at the first effect given as HLSL text, which is plan 9's (the HLSL compiler, with its library choice under spec 5.2 and 9).
2. **Rebased, not replaced:** plan 5's `dinput8.c` keeps its objects, its setup answers and `BW2_QUIT_AFTER_READS`. Only the reads and `SetDataFormat`'s object matching change, and the "no input" notice goes.
3. **The mouse only.** The exe holds no keyboard or joystick GUID, so `CreateDevice` of anything but the mouse, `EnumDevices`, `GetCapabilities` and the other methods the game never calls stay loud. The keyboard is window messages, as the game reads it.
4. **`GetKeyState` does not drain** the host queue; `GetCursorPos`, `GetAsyncKeyState`, the pump and the DirectInput reads do. `GetKeyState` reports the state as of the messages already queued, as Windows' reports it for the messages already retrieved; draining there would put the state ahead of the message the window procedure is handling.
5. **IMM32 as Windows with no IME** on the US layout (the USA release, spec 11): a context can be made, associated and destroyed; it is never open, and the layout has no description or properties. Composition and candidate calls answer "nothing".
6. **`ClipCursor` with an empty or inverted rect clears the clip**, as Windows does. The game passes such a rect twice, built by a failed `GetWindowRect(NULL)`. Since its mouse is exclusive from the front end on, the clip only affects the Win32 cursor and its messages.
7. **`IsDBCSLeadByte` is FALSE** for every byte: code page 1252 has no lead bytes. **`PathGetCharTypeW`** follows shlwapi's documented classes, with non-ASCII characters valid in long and short names.
8. **`BW2_INPUT_SCRIPT`** is a test and measurement switch, like `BW2_QUIT_AFTER_READS`. It pushes into the same host queue the Mac window feeds, so the drain, the messages and DirectInput run exactly as for a person. It is keyed to the read count because the game reads once per frame and the count is stable across loading speeds (plan 8 Ruling 12). The boot test's script holds only coordinates and key codes.
9. **No frame dumps in the engine.** Frames are the game's output; tests keep asserting on `BW2_FRAME_LOG`'s numbers. A spike that needs to see the screen dumps frames on a scratch branch, outside the repo, and keeps nothing.
10. **The screen size is the desktop mode from the start** (1920x1080 headless), where win32hle's default answered 640x480. A Windows game asking `GetSystemMetrics` sees the desktop; the full-screen device's mode replaces it while the device lives.
11. **GUI tests stay opt-in** (`BW2_GUI=1`) and open only a small windowed device for a few seconds. The full-screen path is covered by the pure geometry tests and is tried by hand; no automated run switches Spaces.
12. **The game's own cursor:** with the exclusive mouse the Mac pointer is hidden and detached over the game, and the game draws its cursor from the deltas, 1:1. So a person sees one cursor, the game's, as on Windows.
13. **No new third-party code.** AppKit, CoreGraphics and QuartzCore are Apple system frameworks; nothing needs a licence check under spec 9.
