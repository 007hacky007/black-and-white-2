# Boot to the graphics layer (plan 3)

Measured 2026-11-11 on an Apple M1 Pro. `python -m bw2.pipeline` builds
`out/bw2`, the lifted game linked with pcrecomp's win32hle (headless core: no
SDL2, no ffmpeg) and the BW2 layer in `runtime/bw2/`.

```
BW2_SUPPORT=/tmp/s out/bw2 --game gamefiles/game
```

The run boots the patched `white.exe` and runs its own start-up: the C runtime,
`WinMain`, window creation and the protection VM. It exits with code 4 at its
first call into a later plan's layer:

```
[bw2] reached Direct3DCreate9: owned by a later plan
```

That point is reached in about 1.3 s of wall time.

## What the boot does

`BW2_TRACE_IMPORTS=1 HLE_FILETRACE=1 HLE_REGTRACE=1 out/bw2 ... --trace-eh`:

| Measure | Value |
|---|---|
| Distinct imports reached | 98 |
| Import calls | 133,422 (most are the CRT's `TlsGetValue`/`GetLastError`/`SetLastError` per-thread data, 37,600 each) |
| Heap | 7,538 `HeapAlloc`, 5,329 `HeapFree`, 4,926 `HeapSize` |
| Threads created | 0 |
| C++ exceptions thrown | 0 |
| Protection VM | 163 dispatcher entries (its `GetCurrentThreadId` calls). The VM made no other import call, and nothing tripped the disc/driver tripwire |
| Window | one class, one window, `ShowWindow`, a message loop pass, `ClipCursor` |

### Order of first calls

1. **CRT start-up:** `GetVersionExA`, `HeapCreate`, TLS, `GetStartupInfoA`, the command line and environment, code pages, locale, and `SetUnhandledExceptionFilter`.
2. **Timers:** `QueryPerformanceCounter` and `GetTickCount`.
3. **The user:** `GetUserNameA`.
4. **Two registry lookups.**
5. **A read of its own exe:** `CreateFileA` of `white.exe`, then `GetFileInformationByHandle`.
6. **The window, and the system settings:** `SystemParametersInfoA` (the mouse speed), `GetDoubleClickTime` and `GetSystemPowerStatus`, then `RegisterClassA`, `CreateWindowExA` and one message loop pass.
7. **The My Documents folders.**
8. **The machine's memory:** `GlobalMemoryStatusEx`, plus the working set read and set.
9. **COM, then Direct3D:** `CoInitialize`, `CoCreateInstance`, and finally `Direct3DCreate9`.

### Registry

The game looks up these keys, finds none of them, and carries on:
- `Software\Lionhead Studios Ltd\Black & White 2\LHVersion\White` (twice)
- `Software\Lionhead Studios Ltd\Black & White`

So nothing is seeded.

### Files

- The game reads its own `white.exe` once.
- It looks for `My Documents\Black & White 2\Settings.ini`. The file is absent on a first run, so the game uses its defaults.
- It lists `Profiles\*.*`.
- It creates `My Documents\Black & White 2\` with `Cache\` and `Profiles\` inside.

Every write lands in the support folder. The game tree is not touched.

### Answers the boot needed

These are BW2's own shims, each checked in `runtime/bw2/shims_selftest.c`:

| Import | Answer |
|---|---|
| `InitializeCriticalSectionAndSpinCount` | win32hle's `InitializeCriticalSection`; TRUE |
| `SystemParametersInfoA` | mouse speed and mouse settings, sticky/toggle/filter keys and the screen saver. Reads give Windows' defaults; writes succeed and change nothing. Any other action stops by name |
| `GetDoubleClickTime` | 500 ms |
| `GetKeyboardLayout`, `GetKeyboardLayoutList` | US English (`0x04090409`) |
| `_TrackMouseEvent` | accepted. The input layer (a later plan) sends the messages |
| `GetSystemPowerStatus` | on AC power, no system battery |
| `HeapSetInformation` | TRUE (a hint) |
| `GlobalMemoryStatusEx`, `GlobalMemoryStatus` | 2 GB of RAM with 1.5 GB free, through both calls. win32hle's 1 GB with 512 MB free is only BW2's recommended amount, and could steer its settings low, like the CPU clock |
| `Get/SetProcessWorkingSetSize` | Windows' defaults; a set succeeds with no effect |

The game also tries these, which fail as they would on a bare Windows, and it
carries on:
- `GetProcAddress` for the `Fls*` functions. The CRT then falls back to TLS.
- `LoadLibraryA("PSApi.dll")`.

### For the later plans

- **`CoCreateInstance(CLSID_DirectDraw)`** (`{D7B70EE0-4340-11CF-B063-0020AFC2CD35}`)
  is called just before `Direct3DCreate9` and answered "class not registered".
  The game most likely uses DirectDraw to read video memory, which may feed its
  detail settings. **Plan 4 must answer it.**
- **The game's detail settings are not chosen before Direct3D.** No
  `Settings.ini` is written on the way, so the measured CPU clock (3 GHz,
  `runtime/bw2/cpu.c`) and memory (2 GB) cannot be tied to a saved setting yet.
  Plan 4 checks what the first run saves.

## What the bring-up found

### Discovery gaps

- **48 functions reached only through a code immediate.** One example is
  `0x414A80`, stored by `mov [reg+0x34], offset` as an object's callback.
  `python -m bw2.extrastarts` finds them: 16-byte aligned, after `int3`
  padding, outside every catalogued function, and named by a 32-bit value in
  `.text`. They are committed as addresses in
  `bw2/catalog/white-1.42.extra-starts.json`.
- **The protection VM's native code beyond the 18 catalogued thunks.**
  - 106 re-entry stubs, `push imm32; jmp 0x242A000`, in `.name0`.
  - 3 exits, `mov eax, imm32; ret`, in `stxt774`.
  - 28 thunks, `jmp <entry>`, in `stxt774`. The game calls 18 of them; the VM
    returns through the rest.

  All 138 are in `bw2/catalog/white-1.42.vm-extra-starts.json`, and the VM
  module lifts 330 functions.

### The VM, run as shipped

The user's decision was to run the VM as shipped. This needed two things the
lift did not do, both checked against the original x86 under Unicorn:

1. **Computed returns.** The interpreter leaves through `pop eax; popal;
   popfd; ret` to an address its bytecode put on the stack: one of the exits
   above, a re-entry stub, or the caller. The VM module is lifted with
   pcrecomp's `computed_ret`: a `ret` that does not find the lift's dummy
   return address jumps to what it finds.
2. **Self-modifying entry stubs.** The VM rewrites the `push` operand of its
   re-entry stubs while it runs: the handler at `0x242A7D4` writes `0x24507E2`
   with the bytecode address to resume at. Lifted with the operand from the
   file (0), the dispatcher fetched bytecode from address 0. Each stub's
   `push` now reads its operand from guest memory when it runs (`bw2/lift.py`
   `runtime_push`).

**Oracle check:** `tests/test_boot.py::test_vm_call_matches_unicorn` calls the
game's first VM function, `0x242917C`, with the arguments the game passes,
both lifted (`out/bw2 --call`) and natively under Unicorn (`bw2/pe32emu.py`).
Both return 1. Natively the call leaves the interpreter three times, through
`0x24293A4`, `0x24507E1` and `0x2428000`.

### Exceptions

EH landing pads cover 9 white functions and 1 in ScriptLibraryR.
- Of the 138 white functions with a `push ebp` frame and try blocks, 129 catch
  only to rethrow, as in `catch (...) { free; throw; }`, and need no
  continuation.
- Two catching functions have no `ebp` frame (`0x92F370`, `0xAF0FD0`). A throw
  they would catch stops the run as uncaught, rather than resuming in the
  wrong place.

The boot throws nothing, so the dispatch is proven only by
`tests/test_engine.py` so far.
