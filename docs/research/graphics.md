# The boot through Direct3D (plan 4)

Measured 2026-11-11 on an Apple M1 Pro, with `out/bw2` built by `python -m bw2.pipeline`
(114,531,320 bytes).

```
BW2_HEADLESS=1 BW2_SUPPORT=/tmp/s out/bw2 --game gamefiles/game
```

The run gets through every Direct3D 9 and DirectDraw question the game asks
before it sets up its input. It exits with code 4 at its first call into a later
plan's layer:

```
[bw2] reached DirectInput8Create: owned by a later plan
```

That point is reached in about 0.3 s of wall time, headless and untraced.

## What the game asks before input

`BW2_TRACE_IMPORTS=1 HLE_FILETRACE=1` gives 1,237,892 import calls to 122 distinct
names. The graphics calls among them:

| Call | Count | Answer |
|---|---|---|
| `CoCreateInstance(CLSID_DirectDraw)`, then `IDirectDraw7::Initialize`, `GetAvailableVidMem` and `Release` | 1 each | 512 MB of local video memory (`runtime/bw2/d3d9/ddraw7.c`) |
| `Direct3DCreate9` | 3 | an IDirect3D9 each time |
| `IDirect3D9::GetAdapterIdentifier` | 2 | "BW2 virtual GPU (Metal)", vendor `0x106B` |
| `IDirect3D9::Release` | 2 | |

The game does not create a device, query caps or formats, or call D3DX before
DirectInput. So plan 4's device, resource, draw and D3DX code is proven by its
selftests (`tests/test_d3d9.py`, `tests/test_d3dx.py`) and not yet by the game.

**For the next plans:** the game sets up DirectInput before it creates its
Direct3D device. Plan 5 answers that setup, and then the game's shaders and
effects. The boot now runs on to plan 7's `D3DXCreateSprite`. What it does on the
way (effects, techniques, shaders, compiles, input) is in `shaders.md`.

## The other imports the boot reached

These are answered as a bare Windows on a standard account would answer them, each
checked in `runtime/bw2/shims_selftest.c`:

| Import | From | Answer |
|---|---|---|
| `LookupPrivilegeValueA`, `OpenProcessToken`, `AdjustTokenPrivileges` | the helper at `0x9B4FE0`, asked by `0x9B5150` for `SeLockMemoryPrivilege` (large pages) | the name looks up and the token opens, but the privilege is not granted (`ERROR_NOT_ALL_ASSIGNED`). The game falls back to normal pages. |
| `WSAStartup` (`ws2_32` ordinal 115) | `0x8645C4` | `WSASYSNOTREADY`: no network stack. The game leaves its network flag off (`0x1D5F244`) and carries on. Online play is a non-goal, and the game opens no host socket. |

The game also asks `GetProcAddress(ntdll.dll, ZwLockVirtualMemory)` and
`ZwUnlockVirtualMemory`. Both are not found, as for a process without them, and the
game carries on.

## Detail settings (the open item from `boot.md`)

- The game reads `Data/graphics.config` (1,186 bytes of text). It is a table from
  substrings of the adapter description to three numbers: resolution and detail
  levels from 1 to 3, and a "min spec" shading level from 0 to 2.
- The function that reads it (`0x9355E0`) ends on every path by storing 3, 3 and 0
  into `0x1D6D850`, `0x1D6D854` and `0x1D6D858`. That is high resolution, high
  detail and high-quality shading. A matching row is parsed but does not change
  them. `0x934F20` returns that block.
- So in this exe the adapter's name does not choose the settings. The made-up
  identity (description, vendor `0x106B`) cannot push the game to low detail
  there.
- Two other inputs are read before the device exists:
  - `VendorId == 0x1002` (ATI) sets a flag at `0x933BB0`. With our vendor, the flag
    comes from a `GetVersionExA` check instead.
  - `PixelShaderVersion >= ps_2_0` (`0x933C0D`) raises a texture stage count to 16.
    Ours was ps_3_0 in plan 4 and is ps_2_0 since plan 5.
- No `Settings.ini` is written before the stop. Its first save comes after input
  setup.

## Behaviour pinned by the selftests

- **Window:** with the engine flag `--selftest-window` (opt-in test
  `test_window_device_presents`, `BW2_GUI=1`), a windowed device on a real
  NSWindow presents 60 frames from the guest's thread. The process main thread
  runs AppKit meanwhile.
- **D3D9 semantics:**
  - a pixel's centre is at integer coordinates;
  - the default cull mode removes counter-clockwise triangles on screen;
  - render targets persist across Present, and new resources read as zero;
  - `Reset` refuses while DEFAULT-pool resources live.
- **The fixed-function pipeline:** each D3DTOP, blend factor, Z function, fog mode
  and light type the tests cover gives the documented result, within 2 of 255.
- **Shader cache:** a second device compiles nothing new. After a reload from
  `<support>/ShaderCache/ff.metalar`, every pipeline comes from the archive.
