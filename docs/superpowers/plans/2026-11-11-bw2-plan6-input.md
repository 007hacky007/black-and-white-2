# Plan 6: Input and the game window Implementation Plan

**Status (user, 2026-11-11): deferred.** The game reads no input before the shader stage, so shaders and effects come first (plan 5). Plan 5 starts with a minimal DirectInput mouse that answers the game's setup and reports no input. Before this plan runs, update it from what the game does once it reads input, and rebase its Task 2 on plan 5's stub. Below, "plan 6's layer" means the shader layer, which is plan 5's. **Still deferred after plan 7 (2026-11-12):** the game pumps window messages through its loading screen but reads no input before the audio stop (no `GetDeviceData`, `GetDeviceState`, `GetCursorPos`, `GetAsyncKeyState` or `GetKeyState`; `docs/research/startup.md`), so there is still no input read to start from.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game gets past `DirectInput8Create`. Its mouse is answered by a DirectInput 8 device, and its keyboard and mouse messages, cursor calls and IME calls are answered as Windows answers them. All of these are fed from the Mac window's events, with the trackpad mapping of spec 7.4. The window shows windowed or full screen with Cmd+Ctrl+F to switch, and Cmd-Q or the close button reach the game as `WM_CLOSE`. The boot then stops at the first call into the shader and effect layer, now plan 6's.

**Architecture:**
- The Mac side (main thread) turns each `NSEvent` into a small C struct. A pure C translator turns that struct into input events in the game's terms: client coordinates in back-buffer pixels, Windows virtual keys and scan codes, wheel units and mouse deltas. The events go on a locked host queue.
- The guest thread drains that queue whenever the game could see input: when its message queue is empty (win32hle's pump hook), and at every DirectInput read, `GetCursorPos`, `GetKeyState` and `GetAsyncKeyState`. Draining feeds two consumers from the same events: win32hle's message queue (`hle_input_*`) and the buffers of the acquired DirectInput devices.
- DirectInput 8 objects are guest COM objects built with plan 4's `d3d_com` machinery (new kinds and interface tables). Unimplemented methods stop loudly, as in plan 4.
- The cursor policy (hidden, detached from the pointer, which image, the clip) is computed in C from the game's calls and the window's state. The Mac side only applies it.
- Phasing change (my call; spec 11 leaves phasing to the builder): the game sets up DirectInput before its Direct3D device, so input is plan 5. Shaders, effects, sprite, font and mesh move to plan 6.

**Tech Stack:** C and Objective-C (clang, arm64, ARC, macOS 13+), AppKit, CoreGraphics (`CGWarpMouseCursorPosition`, `CGAssociateMouseAndMouseCursorPosition`), pcrecomp win32hle (vendored, MIT), Python 3.14 and pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 2 goals, 5.2 Input, 7.4 Input and display, 7.5 AppKit basics). Facts: `docs/research/graphics.md` (where plan 4 stops).

## Measured before writing (2026-11-11)

- **Imports:** DINPUT8 gives only `DirectInput8Create` (IAT `0xC12048`, called through the thunk `0xAD4340`). The exe holds `GUID_SysMouse` (`0xD06188`), `IID_IDirectInput8A` (`0xD061C8`) and `GUID_XAxis` (`0xD061B8`). It holds no `GUID_SysKeyboard`, no joystick GUID and no force-feedback GUID. So DirectInput is used for the mouse only, and the keyboard comes from window messages.
- **Setup (`0x9AF220`):**
  1. `DirectInput8Create(hinst, 0x800, IID_IDirectInput8A, 0x1D7A658, NULL)`.
  2. `CreateDevice(GUID_SysMouse, 0x1D7A65C, NULL)` (slot 3).
  3. `SetDataFormat(0xD118F4)` (slot 11). The format there is the game's copy of `c_dfDIMouse`: dwSize 24, dwObjSize 16, dwFlags 2 (`DIDF_RELAXIS`), dwDataSize 16, 7 objects.
  4. `SetCooperativeLevel(hwnd at 0x1D73184, 6)` (slot 13): foreground, non-exclusive.
  5. `SetProperty(DIPROP_BUFFERSIZE, {20, 16, 0, DIPH_DEVICE, 0x200})` (slot 6): 512 buffered events.
  6. `Acquire` (slot 7).
  Any failure returns 2 from the function.
- **Mode switch (`0x9AF790`):** `Unacquire`, then `SetCooperativeLevel(hwnd, mode)`, then `Acquire`. It is called with 5 (foreground, exclusive) from both read paths (`0x9AFD7B`, `0x9B00C9`). A flag at `+0x176` forces 5.
- **Reads:**
  - `0x9AFDD0`: `GetDeviceData(20, buf, &n, 0)` with `n = 0x200`. On `DIERR_INPUTLOST` (`0x8007001E`) or `DIERR_NOTACQUIRED` (`0x8007000C`) it calls `Acquire` and retries. Any other failure leaves the read.
  - `0x9B00EA`: `GetDeviceState(16, buf)`.
  - `0x9AF210`: `Unacquire` at shutdown.
- **Window class (`0x9B11C0`):** style `0xB`, which is `CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS`. So the game gets double-click messages.
- **user32 imports** that touch input: `GetCursorPos` (1 site), `SetCursorPos` (1 site, `0x9AFA11`, in the mouse code), `ClipCursor` (2 sites, around `SetWindowPos`), `ShowCursor` (1), `SetCursor` (4), `GetAsyncKeyState` (7), `GetKeyState` (1), `GetKeyNameTextA/W`, `GetKeyboardLayout`, `GetDoubleClickTime`.
- **IMM32 imports:** 14. win32hle answers 7 of them (`ImmAssociateContext`, `ImmGetCandidateListA`, `ImmGetCompositionStringA`, `ImmGetContext`, `ImmNotifyIME`, `ImmReleaseContext`, `ImmSetOpenStatus`). Unanswered: `ImmCreateContext`, `ImmDestroyContext`, `ImmGetDescriptionA`, `ImmGetOpenStatus`, `ImmGetProperty`, `ImmSetCompositionStringA`, `ImmSetCompositionWindow`.
- **A throwaway run with a do-nothing mouse** (not kept):
  - The boot gets past input. It creates a **full-screen** device: 1024x768, `D3DFMT_A8R8G8B8`, one back buffer, `D3DSWAPEFFECT_DISCARD`, hwnd = focus = `0x10010`, auto depth `D3DFMT_D24S8`, 60 Hz, `D3DPRESENT_INTERVAL_IMMEDIATE`. Before that it calls `AdjustWindowRect`, `SetWindowPos` and `ShowWindow`. After it, it calls `SetMenu`, `SetWindowLongA` twice, `SetWindowPos` twice, `GetWindowRect` and `ClipCursor`.
  - It stops (exit 4) at `D3DXCreateEffect`, called from `0xB5237E`.
  - **It never pumps messages or reads the mouse before that stop.** So this plan's message, read and cursor paths are proven by the selftests, not by the game. The game first reaches them in plan 6.
- **win32hle today:**
  - `hle_input_mouse`, `hle_input_key`, `hle_input_cursor`, `hle_set_pump_hook`, `hle_cursor_hook`, `hle_cursor_image` and `hle_set_screen_size` exist.
  - No wheel input.
  - No double-click synthesis.
  - No extended-key bit and no repeat bit in key messages.
  - Modifier keys are posted with the left/right VK (`0xA0`) where Windows posts the generic one (`VK_SHIFT`).
  - `ClipCursor` does nothing.
  - The engine never calls `hle_set_screen_size`, so `GetSystemMetrics(SM_CXSCREEN)` answers win32hle's default of 640.
- **Plan 4's present** already letterboxes the back buffer into the drawable (`mtl_device.m`, the viewport at line 581). The window is made at the back buffer's size in points, uncapped.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings, bytes, or generated C. Facts only (addresses, sizes, hashes). Run `third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The tripwire stays on, and a tripped run is reported, never worked around. Its allow list stays empty.
- **Network (user, 2026-11-11):** Winsock keeps answering "no network".
- Fail loudly, never return 0 silently:
  - An unimplemented DirectInput method stops with `d3d9: <Interface>::<Method> not implemented (from guest 0x%08X)`, as plan 4's are. The message prefix is plan 4's shared one.
  - `CreateDevice` of a GUID this plan does not implement (keyboard included) stops loudly, naming the GUID. Joystick and game-controller GUIDs get `DIERR_DEVICENOTREG`, because none is attached.
  - An unknown `SetProperty` / `SetCooperativeLevel` value stops loudly, naming it.
- Plan 6 owns the shader and effect names plan 4 listed: device `CreateVertexShader` and `CreatePixelShader`, and D3DX `D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXAssembleShader`, `D3DXCreateEffect`, `D3DXCreateEffectPool`, `D3DXCreateFragmentLinker`, `D3DXCreateSprite`, `D3DXCreateFontA`, `D3DXCreateSphere`, `D3DXGetShaderVersion`, `D3DXGetVertexShaderProfile` and `D3DXGetPixelShaderProfile`. Each still exits 4 with `[bw2] reached <name>: owned by a later plan`. DirectSound/OpenAL and Bink stay as in plan 3. `dinput8.dll` leaves `later.c`.
- Exit codes are unchanged: 0 normal, 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- **Headless:** `BW2_HEADLESS=1` means no NSApplication and no window, as in plan 4. Every test runs headless except the opt-in GUI test (`BW2_GUI=1`). Headless, the window counts as active from the start, so a foreground device can be acquired.
- **Threads:** the Mac side never touches guest memory or win32hle state. It only pushes to the host queue (`bw2_in_push`, which takes its own mutex). Draining happens on the guest thread, under the machine.
- **Mapping decisions (spec 7.4; my calls where the spec leaves them):**
  - Left, right and middle buttons as on Windows. Other buttons 3 and 4 (`NSEvent.buttonNumber` 3, 4) go to DirectInput buttons 3 and 4 only, with no `WM_XBUTTON*` message.
  - The trackpad's two-finger click arrives from macOS as a right-button event and is used as one. Control+click is not a right click.
  - Wheel: a wheel mouse's line delta gives 120 per line. A trackpad's precise delta gives one notch of 120 per `BW2_SCROLL_POINTS` = 10 points, with the remainder carried. Only whole notches are sent. Momentum events are ignored. The direction is `scrollingDeltaY` as macOS delivers it, so it follows the user's natural-scrolling setting. Horizontal scroll is ignored.
  - Option + left drag = middle button. If the game thinks Alt is down at that press, it first gets Alt up. Option's key events are then held back until Option is released. The middle button goes up when that left button goes up, whatever the modifiers are then.
  - Cmd is never sent to the game. A key pressed with Cmd held goes to the menus. Caps Lock is sent as `VK_CAPITAL` down and up on each toggle. Fn and keypad Clear are not sent.
  - Mouse deltas for DirectInput are the event's `deltaX`/`deltaY` in points, divided by the letterbox scale (view points per back-buffer pixel), with fractions carried. So the game's own cursor keeps up with the Mac pointer.
  - On deactivation, every key and button the game thinks is down gets its up message first. DirectInput foreground devices are lost. The D3D device is not lost: Cmd-Tab does not force the game through `Reset`.
  - `ClipCursor`: the reported coordinates are clamped to the clip rect, and the real pointer is never trapped. While the window is inactive, nothing is clamped.
  - Exclusive DirectInput mouse, while acquired and active: the pointer is hidden and detached (`CGAssociateMouseAndMouseCursorPosition(false)`). No `WM_MOUSE*` messages are sent and the Win32 cursor position does not move, as on Windows XP, where exclusive mode suppresses legacy mouse messages.
  - Full screen is macOS full screen (`toggleFullScreen:`, its own Space). Windowed is a resizable window whose first size is the back buffer in points, scaled down to fit 90% of the screen's visible frame. Either way the back buffer is letterboxed into the view.
  - A full-screen device starts in Mac full screen; a windowed one starts windowed. Cmd+Ctrl+F (View > Enter Full Screen) switches at any time without telling the game.
  - The game's screen size (`hle_set_screen_size`, `GetSystemMetrics`, `GetAdapterDisplayMode`) is the desktop mode, or the device's back buffer while a full-screen device lives, as a Windows mode change gives.
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES` (path relative to `third_party/pcrecomp` - what - why). No NFSU2Recomp code. DirectInput is written from its documentation.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Identity is 007hacky007. Never push. Work on branch `plan5`.

## Review Focus

- **Focus lost mid-drag** (Cmd-Tab with a button and a key held): the game gets the ups, and DirectInput reports `DIERR_INPUTLOST` once, then `DIERR_NOTACQUIRED`. `Acquire` gives `DIERR_OTHERAPPHASPRIO` until the window is active again. Nothing stays stuck. (Task 2, group `focus_loss`.)
- **DirectInput buffer overflow** (more events than `DIPROP_BUFFERSIZE` between reads): the oldest are dropped. The next read returns `DI_BUFFEROVERFLOW` (1) once, and sequence numbers keep increasing. (Task 2, group `di_overflow`.)
- **Letterboxed full screen at another aspect** (1024x768 back buffer in a 1512x982 view): the top-left of the picture gives client (0,0), the bottom-right gives (1023,767), and points in the black bars clamp to the nearest edge. (Task 3, group `geom_letterbox`.)
- **Tiny precise trackpad deltas, and momentum:** notches only in whole 120s, the remainder carried, momentum ignored, the sign as delivered. (Task 3, group `scroll_precise`.)
- **Option released before the button in an Option+drag:** the middle button still goes up exactly once, Alt ends up released, and no left button is ever sent. (Task 3, group `option_middle`.)

---

### Task 1: The input queue, and Windows messages from it

**Files:**
- Create:
  - `runtime/bw2/input/input.h`: the event struct, queue, drain and geometry API for the whole plan;
  - `runtime/bw2/input/input.c`: queue, drain, the Win32 side and the activation sequence;
  - `runtime/bw2/input/imm32.c`: the seven unanswered IMM32 calls;
  - `runtime/bw2/input/input_selftest.c`: groups run as `input_selftest <group>`.
- Modify:
  - `bw2/build.py`: `BW2_SUBDIRS = ("d3d9", "d3dx", "input")`;
  - `runtime/bw2/register.c`: register `bw2_input_shims` (BW2's own, before win32hle) and install the pump hook;
  - win32hle `third_party/pcrecomp/runtime/win32hle/user32.c` and `win32hle.h`. These are three separate patch commits, each with a PATCHES line:
    1. `hle_input_key_ex(int down, uint32_t vk, uint32_t scan, int extended)`: bit 24 for extended keys, and bit 30 on a repeat (a down for a key already down). The message's wParam is the generic `VK_SHIFT`/`VK_CONTROL`/`VK_MENU` for the left and right modifier VKs. The key state still records both the side and the generic key. `hle_input_key` calls it with `extended = 0`.
    2. `hle_input_wheel(int delta, int screen_x, int screen_y)`: `WM_MOUSEWHEEL` (0x020A) to the focus window, else the active one. wParam = `(delta << 16) | key state`, lParam = screen coordinates.
    3. Double clicks in `hle_input_mouse`: a second down of the same button within `GetDoubleClickTime` (500 ms) and within 4 px (`SM_CXDOUBLECLK`) of the first, in a window whose class has `CS_DBLCLKS` (8), posts `WM_xBUTTONDBLCLK` instead. A third press starts over. Also `int hle_client_to_screen(uint32_t hwnd, int *x, int *y)` (0 if not a window), which uses the same origin as `ClientToScreen`.
- Test: `tests/test_input.py` (new), with a module fixture that compiles `input_selftest` like `tests/test_d3d9.py` does. Each test runs one group headless and asserts exit 0 and `"<group>: 0 failed"`.

**Interfaces:**
- Produces (`input.h`):
  - `enum bw2_in_kind { BW2_IN_MOVE, BW2_IN_BUTTON, BW2_IN_WHEEL, BW2_IN_KEY, BW2_IN_ACTIVATE, BW2_IN_CLOSE };`
  - `struct bw2_in_event { enum bw2_in_kind kind; uint32_t time_ms; int x, y; int dx, dy; int button; int down; int wheel; uint32_t vk, scan; int extended; int repeat; };`
    - x and y are client coordinates in back-buffer pixels.
    - dx and dy are DirectInput deltas.
    - button: 0 L, 1 R, 2 M, 3, 4.
    - wheel: a multiple of 120.
    - `down` also carries active/inactive for ACTIVATE.
    - time is `hle_ticks_ms()`'s clock, stamped at push.
  - `void bw2_in_push(const struct bw2_in_event *e);`: any thread; a mutex; a growable ring, with no fixed cap.
  - `void bw2_in_drain(void);`: guest thread only.
  - `void bw2_in_set_window(uint32_t hwnd);`: the game window the Mac window shows; until set, `hle_first_hwnd()`.
  - `int bw2_in_active(void);`: 1 until an inactive ACTIVATE drains.
  - `void bw2_in_set_mouse_sink(void (*fn)(const struct bw2_in_event *e));`: Task 2's DirectInput hook, called for every MOVE, BUTTON and WHEEL drained, and for every ACTIVATE.
  - `int (*bw2_in_mouse_exclusive)(void);`: Task 2 sets it. While it returns 1, mouse events make no Win32 messages and leave the Win32 cursor where it is.
  - `void bw2_in_clip(const int *rect_or_null);`: the clip in screen coordinates, used by Task 5's `ClipCursor`.
  - `const win32hle_shim bw2_input_shims[];`: grows in Tasks 2 and 5.
- **Drain, per event:**
  - MOVE: screen coordinates = client plus `hle_client_to_screen`, clamped to the clip when active. Then `hle_input_mouse(WM_MOUSEMOVE, ...)`.
  - BUTTON: `hle_input_mouse` with the button's down or up message, buttons 0-2 only.
  - WHEEL: `hle_input_wheel`.
  - KEY: `hle_input_key_ex`.
  - ACTIVATE active, sent with `hle_send` to the game window in Windows' order:
    1. `WM_ACTIVATEAPP` (1, thread id);
    2. `WM_ACTIVATE` (`WA_ACTIVE` 1, 0);
    3. `WM_SETFOCUS`.
  - ACTIVATE inactive:
    1. up messages for every key and button down;
    2. `WM_ACTIVATE` (`WA_INACTIVE` 0, 0);
    3. `WM_KILLFOCUS`;
    4. `WM_ACTIVATEAPP` (0, thread id).
  - CLOSE: `hle_post_message(window, WM_CLOSE, 0, 0)`.
- **Shims:**
  - `GetCursorPos`, `GetKeyState` and `GetAsyncKeyState` drain, then call win32hle's (`bw2_hle_shim`).
  - The pump hook drains.
- **IMM32, answered as Windows without an IME:**
  - `ImmCreateContext` returns a new nonzero handle; `ImmDestroyContext` returns TRUE.
  - `ImmGetOpenStatus` returns FALSE.
  - `ImmGetDescriptionA` returns 0 and writes nothing.
  - `ImmGetProperty` returns 0.
  - `ImmSetCompositionStringA` returns FALSE.
  - `ImmSetCompositionWindow` returns TRUE.

- [ ] **Step 1: Write the failing tests.** Groups in `input_selftest.c`, each a test in `tests/test_input.py` (`test_<group>`). A host-side recorder on `hle_proc_hook`, or `hle_msg_pop` on a window made with a recording class, checks the messages. Groups:
  - `queue`:
    - 10,000 events pushed from a second pthread arrive in order with nothing lost;
    - draining an empty queue is a no-op.
  - `mouse_msgs`:
    - a window at screen (100,50) made with `CS_DBLCLKS`: MOVE to client (10,20) gives `WM_MOUSEMOVE` with lParam `(20<<16)|10`, and `GetCursorPos` gives (110,70);
    - L down/up/down within 500 ms and 4 px gives `WM_LBUTTONDOWN`, `WM_LBUTTONUP`, `WM_LBUTTONDBLCLK`;
    - the same without `CS_DBLCLKS` gives a second `WM_LBUTTONDOWN`;
    - a press 600 ms later gives `WM_LBUTTONDOWN`.
  - `wheel_msgs`: WHEEL -240 gives `WM_MOUSEWHEEL` with `HIWORD(wParam) == (uint16_t)-240` and lParam in screen coordinates.
  - `key_msgs`:
    - `VK_LEFT` (0x25, scan 0x4B, extended) down gives `WM_KEYDOWN` with lParam `0x014B0001`;
    - a repeat gives `0x414B0001`;
    - the up gives `0xC14B0001`;
    - left Shift (vk `0xA0`, scan 0x2A) down gives wParam `VK_SHIFT` (0x10), and `GetKeyState(VK_LSHIFT)` and `GetKeyState(VK_SHIFT)` both read down;
    - Alt+F4 gives `WM_SYSKEYDOWN`;
    - `TranslateMessage` on Shift+`A` gives `WM_CHAR` 'A'.
  - `activate`:
    - with L and `A` held, an inactive ACTIVATE gives `WM_LBUTTONUP`, `WM_KEYUP` 'A', then `WM_ACTIVATE` 0, `WM_KILLFOCUS`, `WM_ACTIVATEAPP` 0, in that order, and `bw2_in_active()` is 0;
    - an active one gives `WM_ACTIVATEAPP` 1, `WM_ACTIVATE` 1, `WM_SETFOCUS`.
  - `close`: CLOSE posts `WM_CLOSE` to the window set by `bw2_in_set_window`.
  - `drain_points`: an event pushed before `GetAsyncKeyState('A')` is seen by it.
  - `imm`: each of the seven IMM32 calls gives the value above, through `call()` as lifted code would.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_input.py -v`. Expected: FAIL (no `input_selftest.c` sources, or unresolved `bw2_in_*`).
- [ ] **Step 3: Patch win32hle** (patches 1-3 above, one commit each, each with its PATCHES line). After each one, `.venv/bin/pytest tests/test_engine.py -q` passes.
- [ ] **Step 4: Implement** `input.c`, `imm32.c`, and the registration.
- [ ] **Step 5: Run** `.venv/bin/pytest tests/test_input.py tests/test_engine.py tests/test_d3d9.py -q`. Expected: all pass.
- [ ] **Step 6: Commit.** `git commit -m "Add the input queue and Windows input messages"`

### Task 2: DirectInput 8 mouse

**Files:**
- Create: `runtime/bw2/input/dinput8.c`.
- Modify:
  - `runtime/bw2/d3d9/d3d_com.h`: kinds `D3DK_DINPUT8`, `D3DK_DIDEVICE`;
  - `runtime/bw2/d3d9/d3d9_methods.c`: interfaces `IDirectInput8A` and `IDirectInputDevice8A`, in the header's declaration order;
  - `runtime/bw2/later.c`: drop `dinput8.dll` and `DirectInput8Create`;
  - `runtime/bw2/input/input.c` (the `bw2_input_shims` entry `DirectInput8Create`);
  - `input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Consumes: Task 1's `bw2_in_set_mouse_sink`, `bw2_in_mouse_exclusive`, `bw2_in_active`, `bw2_in_drain`. Plan 4's `d3d_new`, `d3d_host`, `d3d_impls`, `d3d_vtable`.
- **Values (from the DirectInput 8 headers):**
  - `IID_IDirectInput8A` {BF798030-483A-4DA2-AA99-5D64ED369700}.
  - `IID_IDirectInputDevice8A` {54D41080-DC15-4833-A41B-748F73A38179}.
  - `GUID_SysMouse` {6F1D2B60-D5A0-11CF-BFC7-444553540000}; `GUID_SysKeyboard` {6F1D2B61-...}.
  - `GUID_Joystick` {6F1D2B70-...}.
  - `GUID_XAxis`/`YAxis`/`ZAxis` {A36D02E0/E1/E2-C9F3-11CF-BFC7-444553540000}; `GUID_Button` {A36D02F0-C9F3-11CF-BFC7-444553540000}.
  - HRESULTs:
    - `DI_OK` 0; `DI_NOEFFECT` = `DI_BUFFEROVERFLOW` = `DI_PROPNOEFFECT` = 1;
    - `DIERR_INPUTLOST` 0x8007001E, `DIERR_NOTACQUIRED` 0x8007000C, `DIERR_OTHERAPPHASPRIO` 0x80070005, `DIERR_ACQUIRED` 0x800700AA;
    - `DIERR_INVALIDPARAM` 0x80070057, `DIERR_DEVICENOTREG` 0x80040154, `DIERR_NOTBUFFERED` 0x80040207, `DIERR_NOINTERFACE` 0x80004002;
    - `DIERR_OLDDIRECTINPUTVERSION` 0x8007047E, `DIERR_BETADIRECTINPUTVERSION` 0x80070481, `CLASS_E_NOAGGREGATION` 0x80040110, `E_NOTIMPL` 0x80004001, `E_HANDLE` 0x80070006.
  - Flags and properties:
    - `DISCL_EXCLUSIVE` 1, `NONEXCLUSIVE` 2, `FOREGROUND` 4, `BACKGROUND` 8, `NOWINKEY` 0x10.
    - `DIPROP_BUFFERSIZE` = (GUID*)1, `DIPROP_AXISMODE` = (GUID*)2 (`DIPROPAXISMODE_ABS` 0, `REL` 1). `DIPH_DEVICE` 0.
    - `DIDF_ABSAXIS` 1, `DIDF_RELAXIS` 2.
    - `DIDFT_RELAXIS` 1, `ABSAXIS` 2, `PSHBUTTON` 4, `TGLBUTTON` 8; instance `(type >> 8) & 0xFFFF`, `DIDFT_ANYINSTANCE` 0xFFFF00; `DIDFT_OPTIONAL` 0x80000000.
    - `DIGDD_PEEK` 1. `INFINITE` 0xFFFFFFFF.
- **The mouse:** 3 axes (X, Y, wheel Z) and 8 buttons (DIMOUSESTATE2's).
- **IDirectInput8A:**
  - `DirectInput8Create(hinst, version, riid, out, outer)`:
    - version 0x800 is accepted; a lower one gives `DIERR_OLDDIRECTINPUTVERSION` and a higher one `DIERR_BETADIRECTINPUTVERSION`;
    - riid must be `IID_IDirectInput8A`, else `DIERR_NOINTERFACE`;
    - a non-NULL outer gives `CLASS_E_NOAGGREGATION`.
  - `CreateDevice`: the mouse, joysticks `DIERR_DEVICENOTREG`, others loud.
  - `EnumDevices`: calls back once per attached keyboard and mouse that matches the type filter, with a `DIDEVICEINSTANCEA` of 580 bytes; joystick classes enumerate nothing.
  - `GetDeviceStatus`: `DI_OK` for the mouse and keyboard GUIDs, `DIERR_DEVICENOTREG` otherwise.
  - `Initialize`: `DI_OK`.
  - Other methods: loud.
- **IDirectInputDevice8A, implemented:** `SetDataFormat`, `SetCooperativeLevel`, `SetProperty` (BUFFERSIZE and AXISMODE; others loud), `Acquire`, `Unacquire`, `GetDeviceState`, `GetDeviceData`. Others loud.
  - `SetDataFormat`:
    - checks dwSize 24, dwObjSize 16, dwDataSize a multiple of 4, and every offset inside it;
    - matches each object to an axis or button by GUID (or NULL) and type and instance, in order, each device object used once;
    - an object with no match gives `DIERR_INVALIDPARAM` unless `DIDFT_OPTIONAL`.
  - `SetDataFormat`, `SetCooperativeLevel` and `SetProperty` while acquired give `DIERR_ACQUIRED`.
  - `SetCooperativeLevel` needs one of exclusive/non-exclusive and one of foreground/background. Mouse exclusive+background gives `E_NOTIMPL`. An hwnd that is not a top-level window gives `E_HANDLE`.
  - `Acquire`:
    - no format: `DIERR_INVALIDPARAM`;
    - foreground while inactive: `DIERR_OTHERAPPHASPRIO`;
    - already acquired: `DI_NOEFFECT`;
    - success empties the buffer and the state.
  - `Unacquire` when not acquired: `DI_NOEFFECT`.
  - Reads:
    - not acquired: `DIERR_NOTACQUIRED`;
    - after a loss: `DIERR_INPUTLOST` once (the device is then unacquired).
  - `GetDeviceState(cb, data)`:
    - cb must equal dwDataSize, else `DIERR_INVALIDPARAM`;
    - relative axes report the motion since the last `GetDeviceState`; absolute axes report the running sum;
    - buttons read 0x80 when down.
  - `GetDeviceData(cb, rgdod, &n, flags)`:
    - cb must be 16 or 20, else `DIERR_INVALIDPARAM`;
    - no buffer size set: `DIERR_NOTBUFFERED`;
    - each element is `{dwOfs, dwData, dwTimeStamp, dwSequence[, uAppData = 0]}`; axis data is the delta (or the absolute value); button data is 0x80 or 0;
    - X and Y from one event share a sequence number, and sequence numbers are global across devices and increase;
    - rgdod NULL with n `INFINITE` flushes and gives the count; `DIGDD_PEEK` keeps the data;
    - overflow drops the oldest and gives `DI_BUFFEROVERFLOW` on the next read.
  - The sink records events only while the device is acquired. A foreground device loses acquisition on an inactive ACTIVATE.
  - `bw2_in_mouse_exclusive` returns 1 while any exclusive mouse is acquired and the window is active.

- [ ] **Step 1: Write the failing tests** (groups and tests in `tests/test_input.py`):
  - `di_create`: the version, riid and outer rules; `CreateDevice` of the mouse; a joystick gives `DIERR_DEVICENOTREG`; `EnumDevices` counts 2 for all devices and 0 for joysticks.
  - `di_game_setup`: the measured sequence exactly, with the format built as the game's copy is (24, 16, 2, 16, 7 objects: `{&GUID_XAxis, 0, 0x00FFFF03}`, `{&GUID_YAxis, 4, 0x00FFFF03}`, `{&GUID_ZAxis, 8, 0x80FFFF03}`, then `{NULL, 12..15, 0x00FFFF0C}` for buttons 0-1 and `0x80FFFF0C` for buttons 2-3, all with dwFlags 0). Every call returns `DI_OK`. Then `0x9AF790`'s switch (`Unacquire`, coop 5, `Acquire`) returns `DI_OK` too.
  - `di_data`:
    - after setup, a MOVE (dx 3, dy -2), a BUTTON 0 down and a WHEEL -120 give `GetDeviceData(20, ...)` = 4 elements;
    - X (ofs 0, 3) and Y (ofs 4, -2) share a sequence number; button (ofs 12, 0x80); Z (ofs 8, -120);
    - the timestamps are the push times.
  - `di_state`: two MOVEs (3, 4) give `GetDeviceState(16)` X 7; a second read with no motion gives X 0; a button held reads 0x80.
  - `di_overflow`: buffer size 4 and 6 button events give 4 elements (the newest 4) and `DI_BUFFEROVERFLOW`; the next read gives `DI_OK`.
  - `focus_loss`:
    - after an inactive ACTIVATE: reads give `DIERR_INPUTLOST`, then `DIERR_NOTACQUIRED`;
    - `Acquire` gives `DIERR_OTHERAPPHASPRIO`;
    - after an active ACTIVATE, `Acquire` gives `DI_OK`;
    - the game's retry loop, played back (read, on `INPUTLOST`/`NOTACQUIRED` Acquire, read again), ends with no error.
  - `di_exclusive`: coop 5 acquired: a MOVE makes no `WM_MOUSEMOVE` and `GetCursorPos` stays put, but DirectInput gets it; after `Unacquire`, a MOVE makes a message again.
  - `di_loud`: `GetCapabilities` stops with `IDirectInputDevice8A::GetCapabilities not implemented`; `CreateDevice(GUID_SysKeyboard)` stops naming the GUID.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement** `dinput8.c`, the interfaces and kinds, and the `later.c` change.
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py tests/test_d3d9.py tests/test_engine.py -q`. Expected: all pass. `tests/test_boot.py::test_boot_reaches_input` now fails, as it should (Task 6 replaces it).
- [ ] **Step 5: Commit.** `git commit -m "Add the DirectInput 8 mouse"`

### Task 3: Mac events in the game's terms

**Files:**
- Create: `runtime/bw2/input/translate.c` (pure C, no AppKit).
- Modify: `input.h`, `input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Produces:
  - `struct bw2_mac_ev { int type; double x, y; double dx, dy; int button; unsigned keycode; uint64_t mods; int precise; int momentum; int repeat; double scroll_dy; };`
    - type is one of `BW2_MAC_DOWN`, `UP`, `MOVE`, `SCROLL`, `KEYDOWN`, `KEYUP`, `FLAGS`.
    - x and y are view points with a bottom-left origin (AppKit's).
    - mods are `NSEventModifierFlags` bits: Shift 1<<17, Control 1<<18, Option 1<<19, Command 1<<20, CapsLock 1<<16.
  - `struct bw2_geom { double view_w, view_h; uint32_t bb_w, bb_h; };`
  - `int bw2_translate(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m, struct bw2_in_event *out, int max);`: returns the number of events written (0..4). `struct bw2_translator` holds the carried state: the scroll remainder, the delta fractions, Option-middle state, held-back Option, Alt as the game sees it, and Caps Lock.
  - `int bw2_keymap(unsigned mac_keycode, uint32_t *vk, uint32_t *scan, int *extended);`: 0 for keys that are not sent.
  - `void bw2_letterbox(const struct bw2_geom *g, double *x0, double *y0, double *scale);`: the picture's rect in view points. The scale is min(view_w/bb_w, view_h/bb_h), and the picture is centred.
- Client coordinates: `cx = floor((x - x0) / scale)`, `cy = floor((view_h - y - y0) / scale)`, each clamped to `[0, bb - 1]`.
- **Key map (US layout; Mac `kVK_*` code to VK, set-1 scan code, extended):**
  - Letters `A`-`Z` to VK 'A'-'Z', with scans Q10 W11 E12 R13 T14 Y15 U16 I17 O18 P19 A1E S1F D20 F21 G22 H23 J24 K25 L26 Z2C X2D C2E V2F B30 N31 M32.
  - Mac codes: A00 S01 D02 F03 H04 G05 Z06 X07 C08 V09 B0B Q0C W0D E0E R0F Y10 T11 O1F U20 I22 P23 L25 J26 K28 N2D M2E.
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
  - Modifiers:
    - Shift 38 to A0/2A; right Shift 3C to A1/36;
    - Control 3B to A2/1D; right Control 3E to A3/1D extended;
    - Option 3A to A4/38; right Option 3D to A5/38 extended;
    - Caps Lock 39 to 14/3A.
  - Function keys:
    - F1 7A, F2 78, F3 63, F4 76, F5 60, F6 61, F7 62, F8 64, F9 65, F10 6D to VK 70-79, scans 3B-44;
    - F11 67 to 7A/57, F12 6F to 7B/58;
    - F13 69, F14 6B, F15 71 to 7C-7E, scans 64-66.
  - Keypad:
    - 0-9 (52 53 54 55 56 57 58 59 5B 5C) to VK 60-69, scans 52 4F 50 51 4B 4C 4D 47 48 49;
    - `.` 41 to 6E/53, `*` 43 to 6A/37, `+` 45 to 6B/4E, `-` 4E to 6D/4A;
    - `/` 4B to 6F/35 extended, Enter 4C to 0D/1C extended.
  - Navigation, all extended:
    - Home 73 to 24/47, End 77 to 23/4F, Page Up 74 to 21/49, Page Down 79 to 22/51;
    - Forward Delete 75 to 2E/53, Help 72 to Insert 2D/52;
    - arrows Left 7B to 25/4B, Right 7C to 27/4D, Down 7D to 28/50, Up 7E to 26/48.
  - Not sent: Command 37/36, Function 3F, keypad Clear 47, keypad `=` 51, F16-F20.
- **Event rules:**
  - Mouse:
    - DOWN or UP of buttons 0-4 gives BUTTON, plus a MOVE first if the point moved;
    - MOVE gives MOVE (client coordinates and the delta per the Global Constraints);
    - SCROLL per the wheel rule.
  - Keys:
    - KEYDOWN with Command gives nothing;
    - KEYDOWN and KEYUP give KEY through the key map, with `repeat` carried;
    - FLAGS gives KEY down or up for the modifier whose bit changed; Caps Lock gives down then up.
  - Option + left DOWN is middle, with Alt up first if the game has Alt down. Option's KEY events are held back until Option goes up, and that up is then dropped. The matching left UP is middle up.

- [ ] **Step 1: Write the failing tests** (groups and tests in `tests/test_input.py`):
  - `keymap`: every row of the table, plus the not-sent keys giving 0.
  - `geom_letterbox`, with geometry 1512x982 points and a 1024x768 back buffer (scale 1.27864..., bars left and right):
    - the picture's top-left point gives (0,0) and its bottom-right gives (1023,767);
    - (0, 490) in the left bar gives (0, 384);
    - a 1:1 geometry (1024x768 view) gives client = point with y flipped.
  - `deltas`: deltaX 1.0 at scale 2.0 gives dx 0, then 1 on the second event (fractions carried); the sign of deltaY follows AppKit's (down positive in both).
  - `scroll_precise`: precise deltas 3, 3, 3 give nothing, then a 4th (total 12) gives WHEEL +120 with remainder 2; momentum events give nothing; -25 gives -240 with remainder -3 carried; a non-precise 1.0 gives +120.
  - `option_middle`:
    - Option down (game sees Alt down), left DOWN, Option up, left UP gives KEY Alt down, KEY Alt up, BUTTON 2 down, BUTTON 2 up;
    - the Option up is dropped, and no BUTTON 0 appears;
    - a later plain left DOWN gives BUTTON 0.
  - `cmd_keys`: Cmd+Q KEYDOWN gives nothing; a Command FLAGS change gives nothing.
  - `caps`: the Caps Lock FLAGS bit set gives `VK_CAPITAL` down and up; cleared gives down and up again.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement** `translate.c`.
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py -q`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Translate Mac events to game input"`

### Task 4: The game's window on the Mac

**Files:**
- Create: `runtime/bw2/d3d9/mac_window.m`. It holds `BW2View` (the layer-hosting content view), the window delegate and the app delegate.
- Modify:
  - `runtime/bw2/d3d9/mtl_device.m`: `make_window` uses `BW2View` and `bw2_mac_window_open`, and `mtl_ctx_resize` keeps the window as Task 4 says;
  - `runtime/bw2/d3d9/mac_main.m`: the menus (app menu: Hide Cmd-H, Quit Cmd-Q; View: Enter Full Screen, `toggleFullScreen:`, Cmd+Ctrl+F; Window: Minimize Cmd-M) and the app delegate;
  - `runtime/bw2/d3d9/d3d9_device.c` and `d3d9_adapter.c`: the current display mode;
  - `runtime/bw2/main.c`: `hle_set_screen_size` with the desktop mode at start;
  - `input.h`, `input_selftest.c`, `d3d9_selftest.c`, `tests/test_input.py`, `tests/test_d3d9.py`, `tests/test_engine.py`.

**Interfaces:**
- Consumes: Task 3's `bw2_translate`, `bw2_geom`; Task 1's `bw2_in_push`, `bw2_in_set_window`.
- Produces:
  - `void bw2_mac_window_open(struct mtl_ctx *c, uint32_t hwnd, uint32_t bb_w, uint32_t bb_h, int fullscreen);`
  - `void bw2_mac_window_mode(struct mtl_ctx *c, uint32_t bb_w, uint32_t bb_h, int fullscreen);`: on `Reset`.
  - `void d3d_current_mode(uint32_t *w, uint32_t *h);`: the desktop mode, or the full-screen device's back buffer while one lives.
  - `GetAdapterDisplayMode`, `hle_set_screen_size` and plan 4's mode checks read `d3d_current_mode`. `CreateDevice`, `Reset` and the device's last `Release` update it and call `hle_set_screen_size`.
- **BW2View:**
  - It accepts first responder and first mouse. It has a tracking area for `mouseMoved` and `cursorUpdate:`, and the window accepts mouse-moved events.
  - Each mouse, drag, scroll, key and flags event becomes a `bw2_mac_ev`, then goes through `bw2_translate` with the view's current geometry (bounds in points, the back buffer size), then to `bw2_in_push`.
  - `keyDown` never calls `interpretKeyEvents:`, so no press-and-hold accent popup appears.
  - The drawable size follows the view's bounds times the backing scale, on every resize and backing change. Plan 4's present letterboxes into it.
- **Delegates:**
  - `windowShouldClose:` pushes CLOSE and returns NO.
  - Becoming key pushes ACTIVATE 1; resigning key pushes ACTIVATE 0.
  - `applicationShouldTerminate:`, while a game window exists, pushes CLOSE and returns `NSTerminateCancel` (spec 7.5). With none, it returns `NSTerminateNow`.
- The window is resizable, with full-screen collection behaviour `NSWindowCollectionBehaviorFullScreenPrimary`. Its size and initial state follow the Global Constraints.

- [ ] **Step 1: Write the failing tests:**
  - d3d9 group `display_mode` (`tests/test_d3d9.py::test_display_mode`), headless:
    - before any device, `GetAdapterDisplayMode` and `GetSystemMetrics(SM_CXSCREEN)` give 1920x1080;
    - after `CreateDevice` with Windowed 0 at 1024x768, both give 1024x768, and a window that covered the screen now covers 1024x768 (win32hle's rule);
    - `Reset` to Windowed 1 gives 1920x1080 again;
    - a full-screen device's last `Release` gives 1920x1080.
  - input group `window_size` (`tests/test_input.py::test_window_size`), on a pure function `bw2_window_fit(double vis_w, double vis_h, uint32_t bb_w, uint32_t bb_h, double *w, double *h)` from `input.h`:
    - 1024x768 in 1512x945 gives 1024x768;
    - 1920x1080 in 1512x945 gives 1360.8 x 765.45 (90% of the width is the tighter bound; aspect kept).
  - GUI test `tests/test_engine.py::test_window_input` (opt-in, `BW2_GUI=1`), engine flag `--selftest-input-window`:
    - a windowed 640x480 device on a game window made with `CS_DBLCLKS`;
    - synthetic `NSEvent`s posted to the window: left down and up at the view's centre, the `A` key (keyCode 0), a scroll of 3 lines;
    - it prints the messages recorded by `hle_proc_hook`, then `performClose:`.
    - Expected stdout lines: `WM_LBUTTONDOWN 320,240`, `WM_LBUTTONUP 320,240`, `WM_KEYDOWN 41 001E0001`, `WM_MOUSEWHEEL 360`, `WM_CLOSE`, `input window ok`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_d3d9.py::test_display_mode tests/test_input.py::test_window_size -v` and `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py::test_window_input -v`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_d3d9.py tests/test_input.py tests/test_engine.py -q` and `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py -k window -v`. Expected: all pass, including plan 4's `test_window_device_presents`.
- [ ] **Step 5: Commit.** `git commit -m "Give the game a Mac window with input and full screen"`

### Task 5: The cursor

**Files:**
- Create: `runtime/bw2/input/cursor.c` (the policy, pure C).
- Modify:
  - `runtime/bw2/d3d9/mac_window.m`: applies the policy;
  - `runtime/bw2/input/input.c`: shims `ClipCursor`, `SetCursorPos` and `ShowCursor`, and `SetCursor` through `bw2_input_shims`, each calling win32hle's and then updating the policy;
  - `input.h`, `input_selftest.c`, `tests/test_input.py`.

**Interfaces:**
- Consumes: win32hle's `hle_cursor_shown`, `hle_current_cursor`, `hle_cursor_image`; Task 1's `bw2_in_clip`, `bw2_in_active`; Task 2's `bw2_in_mouse_exclusive`.
- Produces:
  - `struct bw2_cursor_mode { int hidden; int detached; uint32_t image; int warp; int warp_x, warp_y; };`
    - image: 0 is the system arrow; else a win32hle cursor handle that has an image.
    - warp: a pending warp, in client coordinates.
  - `void bw2_cursor_policy(struct bw2_cursor_mode *out);`: the guest thread computes it after each cursor shim and each drained ACTIVATE.
  - `void bw2_cursor_publish(void);`: hands the mode to the main thread with `dispatch_async` (a no-op headless).
- **Rules:**
  - hidden = inactive ? 0 : (ShowCursor count < 0, or `SetCursor(NULL)`, or detached).
  - detached = active and `bw2_in_mouse_exclusive()`.
  - `SetCursorPos(x, y)` in screen coordinates becomes a warp to the client point (screen minus the client origin) when active and not detached.
  - `ClipCursor(rect)` calls `bw2_in_clip(rect)`; `ClipCursor(NULL)` clears it. Both return TRUE.
- **The Mac side:**
  - `[NSCursor hide]`/`unhide`, kept balanced with a flag, only while the pointer is inside the view.
  - `CGAssociateMouseAndMouseCursorPosition(!detached)`.
  - A cursor image becomes a 32x32 `NSCursor` from the AND/XOR masks:
    - AND 1, XOR 0: transparent;
    - AND 0, XOR 0: black;
    - AND 0, XOR 1: white;
    - AND 1, XOR 1 (inverted): black.
  - A warp maps the client point to view points through the letterbox, then to global display coordinates, then `CGWarpMouseCursorPosition`. After the warp, it calls `CGAssociateMouseAndMouseCursorPosition(true)` unless detached.
  - On resigning key it always unhides and reattaches.

- [ ] **Step 1: Write the failing tests** (groups and tests in `tests/test_input.py`):
  - `cursor_policy`:
    - `ShowCursor(FALSE)` gives hidden;
    - an inactive ACTIVATE gives not hidden and not detached;
    - coop 5 acquired and active gives detached and hidden;
    - `SetCursor(NULL)` gives hidden;
    - `SetCursor(LoadCursorA(0, IDC_ARROW))` gives image 0, not hidden.
  - `cursor_warp`: a window at screen (100,50): `SetCursorPos(150, 90)` gives warp (50, 40), and `GetCursorPos` gives (150, 90); while detached, no warp.
  - `clip`: `ClipCursor({0,0,640,480})` then a MOVE to client (700, 10) in a window at screen (0,0) gives `WM_MOUSEMOVE` at (639, 10) and `GetCursorPos` (639, 10); after an inactive ACTIVATE, a MOVE is not clamped; `ClipCursor(NULL)` unclamps.
  - `cursor_image`: a 32x32 cursor resource made by the test (AND/XOR rows) gives the RGBA pixels per the mask rule, through a pure `bw2_cursor_rgba(const hle_cursor_image_t *, uint8_t out[32*32*4])`.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_input.py -q` and `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py -k window -v`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Answer the game's cursor calls on the Mac"`

### Task 6: Boot to plan 6's layer

**Files:**
- Create: `docs/research/input.md`.
- Modify:
  - `tests/test_boot.py`: rename `test_boot_reaches_input` to `test_boot_reaches_effects`;
  - the spec: section 5.2 (the plan notes; shaders, effects, sprite, font and mesh are plan 6's) and 7.4 (a "Plan 5" note with the mapping as built);
  - `docs/research/graphics.md`: "For the next plans" points to `input.md`;
  - the comments in `runtime/bw2/later.c` and `runtime/bw2/d3d9/` that say plan 5 for the shader layer now say plan 6 (`grep -rn "plan 5" runtime docs/research`).

**Interfaces:**
- Consumes everything above.
- Produces the measured record plan 6 starts from.

- [ ] **Step 1: Write the failing test.** `test_boot_reaches_effects` runs `out/bw2` headless on the game tree with a fresh `BW2_SUPPORT` and asserts:
  - exit code 4;
  - stderr has `reached D3DXCreateEffect`, or another of plan 6's names;
  - stderr has no `TRIPWIRE`, `trap`, `unresolved`, `unimplemented`, `not implemented` or `uncaught`;
  - `reached DirectInput8Create` is not in stderr.
- [ ] **Step 2: Run it.** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_boot.py -v`. Expected: PASS if Tasks 1-5 answer the game as measured. Otherwise FAIL at the first call they do not.
- [ ] **Step 3: Bring-up loop.** Run `BW2_HEADLESS=1 BW2_TRACE_IMPORTS=1 out/bw2 --game gamefiles/game`.
  - Each loud stop in this plan's layer (input, window, cursor, IME, display mode), or in plan 4's device code, is fixed with a selftest check added to the owning group first, red then green.
  - Each stop outside every plan's layer, and each tripwire, stops the run and goes to the user.
  - Record each fix in the ledger.
- [ ] **Step 4: Measure and write `input.md`:**
  - the DirectInput calls and their arguments, as the game makes them;
  - the window calls around `CreateDevice` (the styles `SetWindowLongA` sets, the rects, `ClipCursor`'s rect);
  - the screen size the game sees before and after the device;
  - whether any message is pumped or any input read before the stop;
  - the time to the stop;
  - the stop itself.

  Also run once without `BW2_HEADLESS` and record whether the window opens in Mac full screen at the device's size before the stop.
- [ ] **Step 5: Run the full suite.** `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan5-suite.txt 2>&1; tail -3 .superpowers/sdd/plan5-suite.txt`. Expected: all pass. Then `BW2_GUI=1 .venv/bin/pytest tests/test_engine.py -k window -v`. Expected: pass.
- [ ] **Step 6: Audit and commit.** `third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through input to the effect layer"`
