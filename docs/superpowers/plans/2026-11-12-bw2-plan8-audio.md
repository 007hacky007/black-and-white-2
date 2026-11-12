# Plan 8: DirectSound audio and the boot to the front end Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game gets past its audio start and boots to its front end, where it plays its menu music and waits for input. This plan adds:
- DirectSound: `IDirectSound`, one streaming `IDirectSoundBuffer` kind, and the private `dsound.dll` device enumeration the game's audio probe asks for;
- a software mixer that plays those buffers through CoreAudio, or through a real-time null sink when headless;
- the Win32 answers the boot needs between audio and the front end;
- a way for a test to end a run that no longer stops by itself (`BW2_QUIT_AFTER_READS`), and an audio log of numbers (`BW2_AUDIO_LOG`).

The boot then runs into the front end and reads the mouse every frame. Input (plan 6) is the next layer.

**Architecture:**
- **The game mixes its own sound.** Its audio library mixes every voice in software and streams the result into one looping DirectSound buffer (16-bit stereo PCM at 44,100 Hz). So the port needs no OpenAL and no 3D or EAX audio on this path: DirectSound here is a ring of guest memory that a host mixer plays out in real time.
- **Host mixer** (`runtime/bw2/audio/ds_mix.c`): a growable table of voices (guest buffer, format, cursor, playing). `ds_mix` adds the playing voices into a float stereo block at 44,100 Hz, under one mutex. It is pure C, so tests drive it directly.
- **Output** (`ds_out.m`): a CoreAudio default-output `AudioUnit` whose render callback calls `ds_mix`. Headless, or when the unit cannot start, a host thread calls `ds_mix` at the real-time rate into nothing (the null sink), so the game's cursors still move. Neither thread ever enters guest code or takes the machine lock.
- **COM** (`dsound.c`): `IDirectSound`, `IDirectSoundBuffer`, the private class factory and `IKsPropertySet` are guest COM objects on plan 4's `d3d_com` machinery. Unimplemented methods stop loudly.
- **Phasing (my call; spec 11 leaves phasing to the builder):** after audio the boot reaches the front end with only three more answers (a file-open rule, key names, one missed function start), so they are in this plan. Input stays plan 6's: the front end is the first place the game reads input, and Task 5 rewrites plan 6's status from what it reads there.

**Tech Stack:** C and Objective-C (clang, arm64, ARC, macOS 13+), AudioToolbox and CoreAudio (Apple system frameworks), pcrecomp win32hle (vendored, MIT), Python 3.14 and pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 2, 5.2 Audio and Files, 7.2, 9, 11). Facts: `docs/research/startup.md` (where plan 7 stops).

## Measured before writing (2026-11-12)

Main at `d131e7c`, `out/bw2` relinked (114,768,072 bytes), headless, fresh support folder:

```
[bw2] reached DirectSound (CoCreateInstance): owned by a later plan (from guest 0xDEAD0000, sub_008B1400)
```

A throwaway spike branch (deleted, nothing kept) answered each stop with the least that let the boot go on, read the game's audio code, and ran into the front end. In order:

1. **The speaker helper.** `sub_008B1400` (slot 1 of the helper object, vtable `0xCF809C`, made at `0x88B935` when the byte at `0x175EE60` is set):
   - `CoInitializeEx`, then `CoCreateInstance(CLSID_DirectSound, NULL, CLSCTX_INPROC_SERVER, IID_IDirectSound {279AFA83-4981-11CE-A521-0020AF0BE560})`;
   - `Initialize(NULL)` (slot 10), then `SetCooperativeLevel(hwnd, DSSCL_PRIORITY = 2)` (slot 6);
   - then `sub_008B14E0` calls `GetSpeakerConfig` (slot 8). It switches on the low byte only (`0x8B1511`, values 1 to 7).

   If any of these fails, the helper is deleted and the game goes on without it. The object is never released.
2. **The output choice.** `sub_0088B530` reads an output type from its configuration (`0x88B5F9`, six cases). The default type 0 (`0x88B697`) asks the probe below. A result above 0 makes the DirectSound output (`sub_0088A8D0`, object of 0x630 bytes, constructor `0x8B39F0`, vtable `0xCFA7E8`). Otherwise it makes the waveOut output (`sub_008B92E0`, 0x644 bytes, whose `waveOut*` calls are at `0x8B9142`-`0x8B958D`).
3. **The probe** (`sub_008B98F0`, through `sub_008B9700`):
   - `GetVersion`, then `LoadLibraryA("dsound.dll")` and `GetProcAddress(.., "DllGetClassObject")`. Neither result is checked: a NULL module or a NULL entry falls through into the call (`0x8B9718`, `0x8B9734`). So `dsound.dll` and its `DllGetClassObject` must exist;
   - `DllGetClassObject(CLSID_DirectSoundPrivate {11AB3EC0-25EC-11D1-A4D8-00C04FC28ACA}, IID_IClassFactory)`, then `CreateInstance(NULL, IID_IKsPropertySet {31EFAC30-515C-11D0-A9AA-00AA0061BE93})`. A failed `CreateInstance` also falls through into a call on the unset pointer;
   - `IKsPropertySet::Get(DSPROPSETID_DirectSoundDevice {84624F82-25EC-11D1-A4D8-00C04FC28ACA}, 7, NULL, 0, &data, 8, &ret)`. Property 7 is the device enumeration with narrow strings. `data` is `{callback 0x8B9670, context}`;
   - the callback reads DataFlow at +4 (render is 0), DeviceId at +8 and the interface string pointer at +0x20, and copies at most 0x103 bytes of that string (`0x8B96BF`-`0x8B96CC`). It does not check the pointer, so a NULL there crashes the game: +0x18, +0x1C and +0x20 must each point to a valid guest string;
   - `sub_008B9640` then tests four Windows 9x flags (set by `sub_008DF6B0` from `GetVersionExA`; all 0 on NT);
   - `sub_008B9800` builds a registry path from the interface string only if it begins with the 4 bytes of the Win32 device prefix `\\?\`; otherwise it gives -1. With -1, or with no registry key, the probe returns 3.
   The spike's device had an interface string without that prefix, so the probe returned 3 and the game made the DirectSound output. No registry key was opened and no device path was touched.
4. **The DirectSound output:**
   - `sub_008B3630` makes a second `IDirectSound` the same way (`Initialize(NULL)`, `SetCooperativeLevel(hwnd, 2)`); it is never released either;
   - `sub_008B3870` calls `CreateSoundBuffer` with a 20-byte (DirectX 3) `DSBUFFERDESC`:
     - flags `0x18000` (`DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS`);
     - 21,504 bytes (5,376 frames, about 122 ms);
     - `WAVEFORMATEX` PCM, 2 channels, 44,100 Hz, 176,400 bytes/s, block 4, 16 bits.

     Two branches were not taken: flags `0x18014` (adds `CTRL3D | LOCHARDWARE`) when its third argument is set (`0x8B38FF`), and a `WAVEFORMATEXTENSIBLE` when the speaker configuration has more than 2 channels (`0x8B3922`);
   - `Lock(0, 0, .., DSBLOCK_ENTIREBUFFER)`, zero fill, `Unlock` (slot 19), then `Play(0, 0, DSBPLAY_LOOPING)` (`sub_008B3AA0`);
   - streaming: `sub_008B3710` calls `GetCurrentPosition` (slot 4) and uses only the play cursor. Free space is (play - its own write offset) mod size. `sub_008B37D0` then locks 512-byte chunks up to the play cursor, and `sub_008B3820` unlocks them;
   - no other buffer method is called: no `GetStatus`, `SetVolume`, `SetPan`, `SetFrequency`, `Stop`, `Restore` or `Release` in a 150 s run. In that run there were 9,846 `GetCurrentPosition` and 50,346 `Lock`/`Unlock` pairs. Over 12 s of an untraced run the game wrote 88.3 k samples per second (44,100 stereo frames).
5. **Threads.** After the buffer, the audio library starts 5 guest threads through `sub_00899460` (one `SetThreadPriority`) and makes semaphores through `sub_008808B0`. The streaming calls run on those threads.
6. **OpenAL is not used.** An OpenAL loader exists (object at `0x1D67C00`, vtable `0xCFA634`): `sub_008B3150` and `sub_008B2D90` try `OpenAL32.dll` from the current folder, then `wrap_oal.dll`, then the system folder. No run called it, and the shipped discs carry no OpenAL (disc 1's only redistributable is DirectX 9). The type-2 output may use it (a settings choice), not seen.
7. **A missed function start.** After `Play`, `sub_00AD9918` (the vector-constructor iterator) calls `0x8B00B0`, which the catalog lacks: `unresolved indirect call at 0x008B00B0`. The catalogued `sub_008B0080` ends at `0x8B00D6`, over it, and `0x8B00B0` follows a `ret 8`, not padding, so `extrastarts` cannot see it. Of the 91 callbacks pushed before the vector iterators (`0xAD9918`, `0xAD98B8`), it is the only one missing. With it added the boot goes on.

   A neighbouring catalog entry is a bad decode: `sub_008B00D6` (`0x8B00D6`-`0x8B0110`) starts inside the 8-byte instruction at `0x8B00D5` of the new function. A byte search of the exe finds no rel32 `call` or `jmp` to `0x8B00D6` and one 4-byte literal of it, in `_PDATA` (`0x2407368`, data, not code). The new start's own lift is unaffected, because `disassemble_function` follows flow on its own. Task 5 records this in `frontend.md` for later catalog work and confirms that no run dispatches to `0x8B00D6`.
8. **Empty sound banks.** The audio library opens 59 files under `Audio/` with `GENERIC_READ` and `OPEN_ALWAYS`. win32hle sends every `OPEN_ALWAYS` to the support folder's overlay without copying, so each opened as a new empty file in `<support>/Game/Audio/`. The game read empty banks and mixed silence. With a read-only `OPEN_ALWAYS` of an existing file opened in place, the buffer gets non-silent samples about 22 frames after the first input read (two runs: 22 and 22). After 90 s the support folder held no file at all.
9. **`GetKeyNameTextW`** (not answered: `unimplemented import GetKeyNameTextW`) is called 78 times, with buffer size 64. The call instructions are at `0x64A86A` (in `sub_0064A850`), `0x64A961` and `0x64A9C8` (in `sub_0064A8E0`). Every lParam has bit 25 set (left and right not told apart):
   - 38 have scan code 0;
   - the rest are scan codes 0x0C-0x0F, 0x12-0x14, 0x18, 0x1A-0x1D, 0x1F-0x21, 0x23, 0x26, 0x2C-0x2E, 0x31-0x36, 0x38 and 0x39;
   - extended (bit 24) codes 0x1D, 0x48, 0x49, 0x4B, 0x4D, 0x50 and 0x51.

   Each site zeroes the first wchar, calls W, and if the result is still empty falls back to `GetKeyNameTextA` with a 32-byte buffer. The A call sites are `0x64A8AF`, `0x64A99C`, `0x64AA02`, `0x64AA30` and `0x64AA58`. The spike saw no A call, but its stand-in W answer was not this plan's. Under this plan W answers an unknown code (scan 0, 38 of the 78 calls) with an empty string, so A will run, at least for those 38; Task 5 counts the A calls. win32hle's A version today ignores bits 24 and 25.
10. **The front end.** With the answers above the boot reaches the front end:
    - its init comes about 1.8-2.1 s after start, and the first `GetDeviceData` (plan 5's stub, `sub_009AFD20`) about 3.0-3.4 s after start, at frame 794-1065 (two runs);
    - after that the game reads the mouse about once per frame (4,118 reads in 4,881 frames) and pumps messages (`PeekMessageA`, `sub_009B10D0`). There is no `GetCursorPos`, `GetAsyncKeyState` or `GetKeyState` in 150 s;
    - it ran 120 s with no stop and no loud line. It made 5,105 frames, at 40-50 frames per second headless after the front end appears.
    - The model loader prints 12 debug lines naming model and texture files it does not find. Nothing else changes, and they are recorded for later plans.
    - There is no Bink call, no tripwire, and no disc, volume or driver question. Nothing from the protection VM appears on the way.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings or bytes copied from them, sounds, samples, frames, or generated C. Facts only (addresses, sizes, counts, hashes, GUIDs of Microsoft interfaces). No test or log writes the game's audio to disk; `BW2_AUDIO_LOG` prints numbers only. Run `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data.
- Docs (this plan, `frontend.md`, the spec) never quote the game's own output: no debug log lines or strings. They describe such a line in neutral words with the address that prints it. Test sources never contain the game's strings either; they assert on the engine's own lines.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The tripwire stays on, and its allow list (`runtime/bw2/guard.c` `g_allowed`) stays empty. The device enumeration answered here gives no `\\?\` device path, so the game opens no device and no device registry key. Winsock keeps answering "no network".
- Fail loudly, never return 0 silently:
  - Every stop in this plan goes through plan 4's `d3d_fail` with the bare text only, for example `d3d_fail("IDirectSoundBuffer::GetCaps not implemented")`, `d3d_fail("waveOutOpen: the waveOut output is not answered")` or `d3d_fail("dsound: DirectSoundCreate not implemented")`. `d3d_fail` itself adds the `[bw2] FATAL: d3d9: ` prefix and the ` (from guest 0x..., sub_...)` suffix, so the text never repeats the address. Tests match the bare text as a substring.
  - An unimplemented method stops with `<Interface>::<Method> not implemented`, as plan 4's do.
  - A buffer flag, format, lock flag, play flag, cooperative level, property set or id this plan does not handle stops loudly, naming the value. The exact lists are in Tasks 3 and 4.
- **Later plans' layers** each exit 4 with `[bw2] reached <name>: owned by a later plan`:
  - `LoadLibraryA` of `openal32.dll` or `wrap_oal.dll`, Bink, and plan 7's D3DX stops stay as they are;
  - `dsound.dll` leaves `later.c`'s list, and the DirectSound class stops leave `later.c` (Task 3 registers the real classes);
  - `DirectSoundCreate`, `DirectSoundCreate8` and `DirectSoundEnumerateA` leave `g_later_names`. `dsound.c` answers each with a loud `dsound: <name> not implemented` stop, as no plan owns them. The capture and full-duplex classes get the same kind of stop.
- **`bw2.h` does not change.** Every lifted shard includes it, so a change rebuilds the whole game. A new cross-file function is declared `extern` in the file that uses it, as `register.c` does. `runtime/bw2/d3d9/d3d_com.h` and the headers under `runtime/bw2/audio/` may change; each change costs a 2-minute relink.
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES`. The line gives the path relative to `third_party/pcrecomp`, then what changed and why. No NFSU2Recomp code. DirectSound follows the DirectSound documentation; the private enumeration's layout is the one the game's own code reads ("Measured" item 3). No Wine or ReactOS code is read into this repo.
- **Audio output (my calls, recorded as rulings):** CoreAudio's default output `AudioUnit` (AudioToolbox, an Apple system framework: no third-party licence to check against spec 9). `FRAMEWORKS` gains `"AudioToolbox"` and `"CoreAudio"`. The mixer runs at 44,100 Hz, float, stereo; the unit converts to the device's rate.
- Exit codes are unchanged: 0 normal (including `BW2_QUIT_AFTER_READS`), 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- Headless (`BW2_HEADLESS=1`) for every test, which uses the null sink. Only `test_ds_device` (Task 2) touches the real output, and only when `BW2_AUDIO_DEVICE=1`.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Identity is 007hacky007. Never push. Work on branch `plan8`.
- Never add a remote, never change git config, and never write the user's real name or email anywhere (code, docs, commits, logs).

## Review Focus

- **Release while the mixer reads.** The output thread may be inside `ds_mix` when the game's thread releases a buffer. The voice must leave the mixer under its mutex before its guest memory is freed. Nothing may read the freed block afterwards, and nothing may crash. (Task 2, group `ds_mix_free`, which poisons each block between `ds_voice_free` and `hle_free` and fails if the poison value ever reaches the output.)
- **Quitting while sound plays.** On macOS `hle_exit` ends with `_exit` (`third_party/pcrecomp/runtime/win32hle/kernel32.c`, `HLE_LEAVE`), so `hle_exit`, `ExitProcess` and `BW2_QUIT_AFTER_READS` tear the process down at once: no `atexit` handler or destructor runs, and nothing is unmapped while a render callback still reads. The `atexit` stop covers only `exit()` paths, such as a selftest's `main` returning or a plain `exit(0)` from another thread. `hle_on_exit` is a single pointer that `main.c` already uses for `--selftest-eh-depth`; this plan does not take it over. (Task 2, group `ds_out_exit`, both paths; Task 5's boot test exits 0 with audio playing.)
- **COM objects on several guest threads.** This is the first time `d3d_com` objects are used from several guest threads: the 5 audio threads alongside the render thread. That is safe only while every `d3d_com` touch (refcounts, `d3d_host`, the live count) stays under the machine lock, and no DirectSound method holds the mixer mutex or any `d3d_com` state across `hle_block_begin`. The mixer mutex is a leaf: nothing takes the machine lock while holding it. (Tasks 3 and 4.)
- **The null sink after a stall.** If the null sink's thread itself is descheduled, it must not burst ahead on waking: each wake mixes at most 2,048 frames (well under the 5,376-frame buffer), and a backlog above that is dropped by moving its clock forward. A lap caused by the game's own threads stalling for more than one buffer (122 ms) is accepted DirectSound behaviour: a real device keeps playing too, and the game's free-space count wraps as it would on Windows. (Task 2, groups `ds_out_null` and `ds_out_stall`.)
- **Cursor and data agree.** The play cursor is exactly the bytes the mixer has consumed (mod size, whole blocks). Bytes the game writes between its write offset and the reported play cursor have not been read yet, and bytes at or after the play cursor have not been played. A mixer that consumes ahead of what it reports makes the game overwrite data still to be played: a click every 122 ms. (Task 2, group `ds_mix_cursor`.)
- **A lock that wraps.** `Lock(21000, 1024)` on the 21,504-byte buffer gives two parts, 504 bytes at the end and 520 at the start. A lock past the end or larger than the buffer is `DSERR_INVALIDPARAM`, never a pointer outside the buffer. (Task 3, group `ds_buffer_lock`.)
- **The probe's unchecked calls.** The game does not check `LoadLibraryA("dsound.dll")`, `DllGetClassObject` or `CreateInstance` before calling through them, nor the string pointers in the device description ("Measured" item 3). Each must succeed for the game's own GUIDs and leave a valid pointer, or the game calls or reads address 0. (Task 4, group `ds_private`.)
- **No output device.** On a Mac whose unit cannot start, audio goes to the null sink with one notice line. The game's cursors keep moving at the real rate, and the boot is unchanged. (Task 2, group `ds_out_null`, which forces the failure.)

---

### Task 1: Win32 answers and the missed start on the way to the front end

**Files:**
- Modify (pcrecomp, one commit each with its `third_party/pcrecomp.PATCHES` line):
  - `third_party/pcrecomp/runtime/win32hle/kernel32_ext.c`: `CreateFileA`'s read-only open of an existing file;
  - `third_party/pcrecomp/runtime/win32hle/user32.c`: `GetKeyNameTextA` rules and `GetKeyNameTextW`.
- Modify (BW2): `bw2/catalog/white-1.42.extra-starts.json` (add `[9109680, "boot"]`, which is `0x8B00B0`), `runtime/bw2/shims_selftest.c`, `tests/test_extrastarts.py`.

**Interfaces:**
- Consumes: `bw2_mount`, the selftest's `call()` helper and `gmem()`.
- Produces:
  - **`CreateFileA` (kernel32_ext.c):** when the access has no `GENERIC_WRITE`, the disposition is `OPEN_EXISTING` (3) or `OPEN_ALWAYS` (4), and the name resolves to an existing file (`hle_host_path`), that file is opened read-only in place. There is no overlay path and no copy, and `OPEN_ALWAYS` sets `ERROR_ALREADY_EXISTS` as Windows does. A read-only `OPEN_ALWAYS` of a missing file still creates it, in the overlay. All write and create cases are unchanged.
  - **Key names (user32.c),** one table for both functions, Windows' US English layout. scan = bits 16-23, extended = bit 24, don't care = bit 25:
    - letters `A`-`Z` and digits `0`-`9` by scan code as in win32hle's row table. The punctuation keys: 0x0C `-`, 0x0D `=`, 0x1A `[`, 0x1B `]`, 0x27 `;`, 0x28 `'`, 0x29 `` ` ``, 0x2B `\`, 0x33 `,`, 0x34 `.`, 0x35 `/`, 0x56 `\`;
    - 0x01 `Esc`, 0x0E `Backspace`, 0x0F `Tab`, 0x1C `Enter`, 0x1D `Ctrl`, 0x2A `Shift`, 0x36 `Right Shift`, 0x38 `Alt`, 0x39 `Space`, 0x3A `Caps Lock`, 0x3B-0x44 `F1`-`F10`, 0x57 `F11`, 0x58 `F12`, 0x45 `Pause`, 0x46 `Scroll Lock`, 0x37 `Num *`, 0x4A `Num -`, 0x4E `Num +`;
    - not extended, the keypad: 0x47 `Num 7`, 0x48 `Num 8`, 0x49 `Num 9`, 0x4B `Num 4`, 0x4C `Num 5`, 0x4D `Num 6`, 0x4F `Num 1`, 0x50 `Num 2`, 0x51 `Num 3`, 0x52 `Num 0`, 0x53 `Num Del`;
    - extended: 0x47 `Home`, 0x48 `Up`, 0x49 `Page Up`, 0x4B `Left`, 0x4D `Right`, 0x4F `End`, 0x50 `Down`, 0x51 `Page Down`, 0x52 `Insert`, 0x53 `Delete`, 0x1C `Num Enter`, 0x35 `Num /`, 0x1D `Right Ctrl`, 0x38 `Right Alt`, 0x45 `Num Lock`, 0x37 `Prnt Scrn`, 0x5B `Left Windows`, 0x5C `Right Windows`, 0x5D `Application`;
    - with bit 25, `Right Shift` gives `Shift` and `Right Ctrl` gives `Ctrl` (the documentation: bit 25 merges only Ctrl and Shift);
    - an unknown code gives 0 and an empty string. A buffer of n characters takes at most n - 1 and the terminator; the return is the count copied. n = 0 gives 0 and writes nothing;
    - `GetKeyNameTextW` writes the same names as UTF-16.
    - The game calls W first and A only when W gave an empty string ("Measured" item 9), so the unknown codes reach A too, which answers them the same way (0, empty).

- [ ] **Step 1: Write the failing checks** in `runtime/bw2/shims_selftest.c` (run by `tests/test_engine.py::test_shims_selftest`) and in `tests/test_extrastarts.py`:
  - After `bw2_mount(".", "support")` with a file `Audio/a.bin` of 3 bytes `"abc"` written by the check into the mounted game folder:
    - `CreateFileA("Audio\\a.bin", GENERIC_READ, 1, 0, OPEN_ALWAYS, 0x80, 0)` gives a handle, `GetLastError()` 183 (`ERROR_ALREADY_EXISTS`), and `ReadFile` of 8 bytes gives 3 bytes `"abc"`;
    - `support/Game/Audio/a.bin` does not exist afterwards (red today: an empty file is made there);
    - the same with `OPEN_EXISTING` reads `"abc"` (guard: passes today);
    - `CreateFileA("Audio\\new.bin", GENERIC_READ, 0, 0, OPEN_ALWAYS, 0, 0)` creates `support/Game/Audio/new.bin` (guard);
    - `GENERIC_READ | GENERIC_WRITE` with `OPEN_ALWAYS` on `Audio\\a.bin` makes `support/Game/Audio/a.bin` holding `"abc"` (guard: the existing copy rule).
  - Key names (A and W; for W the buffer is read as UTF-16). The W checks first CHECK that `hle_resolve("GetKeyNameTextW")` is nonzero and call it only then. (Review note: the selftest's `call()` already counts an unregistered name as one `not answered` failure, because `hle_resolve` gives 0 and only the game's IAT bind, `hle_resolve_or_stub`, makes the stopping stub; the explicit CHECK keeps that true if a later change binds stubs in the selftest.) So today W fails as one CHECK and does not stop the selftest:
    - lParam `0x02360000` gives `Shift`, `0x00360000` gives `Right Shift`;
    - `0x031D0000` gives `Ctrl`, `0x011D0000` gives `Right Ctrl`;
    - `0x03480000` gives `Up`, `0x02480000` gives `Num 8`;
    - `0x021E0000` gives `A`, `0x02390000` gives `Space`, `0x020C0000` gives `-`, `0x01380000` gives `Right Alt`;
    - `0x02000000` gives 0 and an empty string;
    - `0x000E0000` with n 3 gives 2 and `Ba`.
  - `test_extrastarts.py::test_boot_start_8b00b0`: the committed `white-1.42.extra-starts.json` holds `[0x8B00B0, "boot"]`, and `load_extra_starts()` includes `0x8B00B0`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_engine.py -k shims tests/test_extrastarts.py -v`. Expected: FAIL on each new check (the W part as its one failed resolve CHECK); the guards pass.
- [ ] **Step 3: Implement the `CreateFileA` rule.** Commit: `git commit -m "Open existing files in place for read-only OPEN_ALWAYS"`.
- [ ] **Step 4: Implement the key names.** Commit: `git commit -m "Answer GetKeyNameTextW and the extended key names"`.
- [ ] **Step 5: Add the start** to `white-1.42.extra-starts.json` (keep its format: `python -m bw2.extrastarts --write` keeps "boot" rows). Run the full pipeline: `.venv/bin/python -m bw2.pipeline` (about 3.5 minutes). Expected: `[link]` reports one function more than before (49,434).
- [ ] **Step 6: Run** `.venv/bin/pytest tests/test_engine.py tests/test_extrastarts.py -q` and `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: all pass, and the boot still stops at `DirectSound (CoCreateInstance)`.
- [ ] **Step 7: Commit.** `git commit -m "Add the missed start at 0x8B00B0"`

### Task 2: The mixer and the audio output

**Files:**
- Create: `runtime/bw2/audio/audio.h`, `runtime/bw2/audio/ds_mix.c`, `runtime/bw2/audio/ds_out.m`, `runtime/bw2/audio/audio_selftest.c`, `tests/test_audio.py`.
- Modify: `bw2/build.py` (`BW2_SUBDIRS` gains `"audio"`; `FRAMEWORKS` gains `"AudioToolbox"` and `"CoreAudio"`).

**Interfaces:**
- Produces (`audio.h`):
  - `#define DS_OUT_RATE 44100` and `#define DS_LEAD_FRAMES 512` (the write cursor's lead).
  - `struct ds_format { uint32_t rate; uint16_t channels, bits; };` PCM, 1 or 2 channels, 8 (unsigned) or 16 (signed) bits, rate 100 to 200,000.
  - `int ds_voice_new(uint32_t data, uint32_t bytes, struct ds_format f);`: a voice over `bytes` of guest memory at `data`, stopped at position 0; a positive id. The table grows (no fixed size, spec 7.2).
  - `void ds_voice_free(int id);`: after it returns, the mixer never reads `data` again.
  - `void ds_voice_play(int id);` (looping), `void ds_voice_stop(int id);` (the position is kept).
  - `void ds_voice_cursors(int id, uint32_t *play, uint32_t *write);`: play = consumed frames mod the buffer's frames, times the block. write = play + `DS_LEAD_FRAMES` blocks, mod bytes.
  - `int ds_voice_playing(int id);`
  - `void ds_mix(float *out, uint32_t frames);`: `out` gets `frames` interleaved stereo frames at `DS_OUT_RATE`, the clamped sum of the playing voices. Each voice's consumed count rises by the source frames read.
  - `uint64_t ds_frames_mixed(void);`
  - `void ds_out_start(void);`: starts the output once, at the first voice. Headless (`BW2_HEADLESS=1`) uses the null sink. Otherwise it uses the `AudioUnit`; if a step fails it prints `[bw2] audio: no output device (OSStatus <n>), mixing to the null sink` once and uses the null sink. Exit: `hle_exit` (and so `ExitProcess` and `BW2_QUIT_AFTER_READS`) ends in `_exit` on macOS, which tears the whole process down at once with no `atexit` handler or destructor running, so there is no race with the render callback on that path. For `exit()` paths only (a selftest's `main` returning, a plain `exit(0)` from another thread), an `atexit` handler stops the unit (`AudioOutputUnitStop`, which waits for the callback) or sets the null sink's stop flag and joins it. The handler takes no machine lock. With `BW2_AUDIO_LOG=1` it prints `[bw2] audio: output stopped at exit`. `hle_on_exit` is not used: it is one pointer, and `main.c` already owns it for `--selftest-eh-depth`.
  - `void ds_out_force_null_for_test(void);`: makes the next `ds_out_start` behave as if the unit failed (the notice included).
  - `void ds_out_manual_for_test(void);`: makes `ds_out_start` start nothing, so a test drives `ds_mix` itself.
  - `void ds_out_stall_for_test(uint32_t ms);` and `uint64_t ds_out_skipped_frames_for_test(void);`: make the null sink's next wake sleep `ms` first, and give the frames it has dropped.
- **Mixing rules:**
  - 16-bit samples are s/32768; 8-bit are (s - 128)/128. Mono goes to both channels.
  - Source position advances by rate/44,100 per output frame. The sample is linear interpolation between frame k and k+1 (k+1 wraps to 0, as the buffer loops). At rate 44,100 the output equals the source samples exactly.
  - Gain is 1: the game's buffer has no `CTRLVOLUME`, `CTRLPAN` or `CTRLFREQUENCY` ("Measured" item 4).
  - The sum is clamped to [-1, 1] per sample.
- **Output details:**
  - **The unit:** `kAudioUnitType_Output` / `kAudioUnitSubType_DefaultOutput`, with input format Float32 interleaved stereo at 44,100 Hz. The render callback calls `ds_mix`. It allocates nothing, never enters guest code, and takes no lock but the mixer's.
  - **The null sink:** a host thread that every 5 ms mixes due = floor((elapsed - skipped) x 44,100) - `ds_frames_mixed()` frames into a scratch block, at most 2,048 frames per wake (under half the game's 5,376-frame buffer). When due exceeds 2,048 (the sink's own thread was stalled), the excess is dropped by adding it to `skipped`, so the sink never bursts past the game's write offset by itself. A lap caused by the game's threads stalling for more than one buffer is accepted: a real DirectSound device keeps playing, and the game's free-space count wraps as on Windows. `ds_out_skipped_frames_for_test()` gives the dropped count.
- **`BW2_AUDIO_LOG=1`** (read once): after each 44,100 mixed frames `ds_mix` prints `[bw2] audio second <k>: <v> playing, peak <p>, nonsilent <m> of 44100`. Here k counts from 1, v is the number of playing voices, p is the largest |sample| x 32767 (truncated), and m is the frames with any channel nonzero. The print happens after the mutex is released. On the CoreAudio unit that print runs on the render thread, which real-time audio code should not do; it is accepted because the switch is for debugging only and prints once a second (a dropout while it is on is not a bug). Task 3 adds the buffer line.

- [ ] **Step 1: Write the failing tests.** `tests/test_audio.py` builds `audio_selftest` as `tests/test_input.py` builds its selftest and runs groups (`audio_selftest <group>`, output `<group>: 0 failed`). The groups put their data in `hle_alloc` blocks:
  - `ds_mix_exact`: a 16-bit stereo 44,100 voice of 1,000 frames with distinct samples, played. `ds_mix` of 1,100 frames gives frames 0-999 then 0-99 again, each sample exactly s/32768. The play cursor is 100 x 4 bytes, and the write cursor is (100 + 512) mod 1,000 = 612 frames, x 4 (a buffer that is not a divisor of 512, so the lead shows).
  - `ds_mix_formats`: an 8-bit mono voice at 22,050 Hz with samples 128, 192 gives output frames 0, 0.25, 0.5, 0.25, 0 (linear, both channels) for its first five. A 16-bit mono voice puts the same value in both channels.
  - `ds_mix_sum`: two playing voices of 0.75 sum to a clamped 1.0. Stopping one gives 0.75, and its cursor stays where it stopped.
  - `ds_mix_cursor`: after `ds_mix` of n frames from a fresh voice, the play cursor is exactly n frames (mod size) for n in 1, 511, 512, 5,375, 5,376 and 5,377 on a 5,376-frame buffer.
  - `ds_mix_free`: a thread calls `ds_mix` in a loop while the main thread makes, plays and frees 1,000 voices, keeping at most two alive at a time. Each voice's block holds a small known value (16-bit samples of 0x0100). After `ds_voice_free` returns and before `hle_free`, the main thread fills the block with the poison 0x7F7F. The mixing thread checks every output sample of the whole run: no sample may exceed 2 x 0x0100/32768, so neither the poison 0x7F7F/32768 nor any sum that includes it ever appears, and the run has 0 crashes. Writing the poison before `hle_free` keeps it out of the guest heap's quarantine ring (`guest_mem.c` `quarantine`/`really_free`), so a block that is really freed or reused is never clobbered.
  - `ds_out_null`: with `ds_out_force_null_for_test`, `ds_out_start` prints the notice once. A playing voice's consumed frames after 500 ms of wall time are within 10% of 22,050.
  - `ds_out_stall`: on the null sink, `ds_out_stall_for_test(300)`; after the stall the sink's next wake mixes at most 2,048 frames, `ds_out_skipped_frames_for_test()` is above 0, and the voice's consumed count never rises by more than 2,048 between two wakes.
  - `ds_out_exit`, two children, each with a voice playing on the null sink and `BW2_AUDIO_LOG=1`:
    - one calls `hle_exit(0)` from another thread after 100 ms. Its status is 0, and the `output stopped at exit` line is absent (the `_exit` path runs no handler);
    - one calls plain `exit(0)` from another thread after 100 ms. Its status is 0, and the `output stopped at exit` line appears once (the `atexit` path).
  - `ds_audio_log`: with `BW2_AUDIO_LOG=1` and a constant voice of 0.5, mixing 88,200 frames prints `audio second 1: 1 playing, peak 16383, nonsilent 44100 of 44100` and then `second 2`.
  - `test_ds_device` (skipped unless `BW2_AUDIO_DEVICE=1`; not headless): `ds_out_start` with no notice line. A 440 Hz voice plays 1 s, and the consumed frames come out between 39,690 and 48,510.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_audio.py -v`. Expected: FAIL (no sources).
- [ ] **Step 3: Implement `ds_mix.c`, then `ds_out.m`.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_audio.py -v`, then `BW2_AUDIO_DEVICE=1 .venv/bin/pytest tests/test_audio.py -k device -v` on this Mac. Expected: all pass (the device test passes or reports why the unit failed: record which in the task's report). Then `.venv/bin/python -m bw2.pipeline --build-only` (a new subdirectory: about 2 minutes).
- [ ] **Step 5: Commit.** `git commit -m "Add the audio mixer and its CoreAudio output"`

### Task 3: IDirectSound and IDirectSoundBuffer

**Files:**
- Create: `runtime/bw2/audio/dsound.c`.
- Modify:
  - `runtime/bw2/d3d9/d3d_com.h`: kinds `D3DK_DSOUND`, `D3DK_DSBUFFER`, `D3DK_DSPRIVATE`, `D3DK_KSPROPSET`;
  - `runtime/bw2/d3d9/d3d9_methods.c`: interfaces `IDirectSound`, `IDirectSoundBuffer`, and Task 4's two;
  - `runtime/bw2/register.c`: `bw2_audio_register()`, called where `bw2_input_register()` is;
  - `runtime/bw2/later.c`: drop `dsound.dll`, the DirectSound class stops and the three `DirectSound*` names;
  - `runtime/bw2/audio/audio_selftest.c`, `tests/test_audio.py`;
  - `tests/test_boot.py`: the `xfail` mark on `test_boot_to_audio` (Step 4).

**Interfaces:**
- Consumes: Task 2's `ds_voice_*` and `ds_out_start`. Plan 4's `d3d_new`, `d3d_host`, `d3d_impls`, `d3d_fail`; `hle_com_register_class`, `hle_alloc`.
- Produces:
  - `void bw2_audio_register(void);` (in `dsound.c`): the classes, the impls and the shims of Tasks 3 and 4.
  - `IDirectSound`, IID {279AFA83-4981-11CE-A521-0020AF0BE560}. Methods in order: `QueryInterface`, `AddRef`, `Release`, `CreateSoundBuffer`, `GetCaps`, `DuplicateSoundBuffer`, `SetCooperativeLevel`, `Compact`, `GetSpeakerConfig`, `SetSpeakerConfig`, `Initialize`.
  - `IDirectSoundBuffer`, IID {279AFA85-4981-11CE-A521-0020AF0BE560}. Methods in order: `QueryInterface`, `AddRef`, `Release`, `GetCaps`, `GetCurrentPosition`, `GetFormat`, `GetVolume`, `GetPan`, `GetFrequency`, `GetStatus`, `Initialize`, `Lock`, `Play`, `SetCurrentPosition`, `SetFormat`, `SetVolume`, `SetPan`, `SetFrequency`, `Stop`, `Unlock`, `Restore`.
  - Implemented here: `CreateSoundBuffer`, `SetCooperativeLevel`, `GetSpeakerConfig`, `Initialize`; and on buffers `GetCurrentPosition`, `GetStatus`, `Lock`, `Play`, `Stop`, `Unlock`. All other methods stop as not implemented.
- **Threads:** every method runs under the machine lock, as all shims do. No method holds the mixer mutex or any `d3d_com` state across `hle_block_begin`, and the mixer mutex is a leaf (taken last, held briefly, never while asking for the machine lock). The game calls these methods from its 5 audio threads while the render thread uses Direct3D ("Measured" item 5); this is the first plan whose `d3d_com` objects are touched from more than one guest thread.
- **Classes and creation:**
  - `CoCreateInstance` of `CLSID_DirectSound` {47D4D946-62E8-11CF-93BC-444553540000}, with IID `IDirectSound` or `IUnknown`, gives an object that is not yet initialized. `CLSID_DirectSound8`, and any other IID, stop loudly naming the class or IID (`IDirectSound8` is not implemented).
  - The capture classes and `CLSID_DirectSoundFullDuplex` stop loudly (`dsound: DirectSoundCapture not implemented`). So do `DirectSoundCreate`, `DirectSoundCreate8` and `DirectSoundEnumerateA`.
- **`Initialize(guid)`:** NULL or `DSDEVID_DefaultPlayback` {DEF00000-9C6D-47ED-AAF1-4DDA8F2B5C03} gives `DS_OK`. Any other GUID stops loudly, naming it. A second call gives `DSERR_ALREADYINITIALIZED` (0x88780082). Every other method before it gives `DSERR_UNINITIALIZED` (0x887800AA).
- **`SetCooperativeLevel(hwnd, level)`:** 1, 2 or 3 gives `DS_OK`. `DSSCL_WRITEPRIMARY` (4) stops loudly; any other value is `DSERR_INVALIDPARAM` (0x80070057).
- **`GetSpeakerConfig(&c)`:** c = 0x00140004 (`DSSPEAKER_STEREO` with `DSSPEAKER_GEOMETRY_WIDE`, Windows' desktop-speaker default). My call, Ruling 4.
- **`CreateSoundBuffer(desc, &buf, outer)`:**
  - outer must be NULL;
  - dwSize must be 20 or 36, else `DSERR_INVALIDPARAM`;
  - flags may hold only `GETCURRENTPOSITION2` (0x10000), `GLOBALFOCUS` (0x8000), `STICKYFOCUS` (0x4000) and `LOCSOFTWARE` (8). Any other bit stops loudly naming the unknown bits (`PRIMARYBUFFER`, `CTRL3D`, `LOCHARDWARE` and the CTRL flags included);
  - the format must be `WAVE_FORMAT_PCM` (1) with Task 2's ranges and blockAlign = channels x bits/8; anything else (`WAVE_FORMAT_EXTENSIBLE` 0xFFFE included) stops loudly naming tag, channels and bits;
  - bytes must be a multiple of blockAlign, between 4 and 0x0FFFFFFF, else `DSERR_INVALIDPARAM`;
  - the data block is `hle_alloc`'d (zeroed), and the voice is made with `ds_voice_new`. The buffer object holds a reference on its `IDirectSound`. Its last `Release` frees the voice, then the block, then that reference. The first buffer calls `ds_out_start`.
  - With `BW2_AUDIO_LOG=1` it prints `[bw2] audio buffer <n>: <ch> ch, <rate> Hz, <bits> bit, <bytes> bytes, flags 0x<x>`.
- **`Lock(off, bytes, &p1, &n1, &p2, &n2, flags)`:**
  - `DSBLOCK_FROMWRITECURSOR` (1) takes off from the write cursor; `DSBLOCK_ENTIREBUFFER` (2) takes bytes = size. Another flag stops loudly;
  - off >= size, bytes > size, or bytes 0 without `ENTIREBUFFER` give `DSERR_INVALIDPARAM`;
  - n1 = min(bytes, size - off), and p2/n2 hold the wrapped rest (p2 = data, or 0 with n2 0). A NULL p2 pointer with a wrap needed gives only the first part;
  - `Unlock` checks that each nonzero pointer lies in the buffer with its count, else `DSERR_INVALIDPARAM`.
- **`Play(r1, priority, flags)`:** flags must be `DSBPLAY_LOOPING` (1) and r1 and priority 0, else it stops loudly. My call: the game only loops, and one-shot end handling is not built until a call asks. `Stop` stops. `GetStatus` gives `PLAYING | LOOPING` (5) or 0.
- **`GetCurrentPosition(&play, &write)`:** `ds_voice_cursors`; either pointer may be NULL.

- [ ] **Step 1: Write the failing tests** (groups in `audio_selftest`, calling the guest vtables as `input_selftest.c` calls DirectInput's):
  - `ds_create`:
    1. `CoCreateInstance` as the game makes it ("Measured" item 1) gives an object. Before `Initialize`, `CreateSoundBuffer` gives 0x887800AA;
    2. `Initialize(NULL)` gives 0, and a second one gives 0x88780082;
    3. `SetCooperativeLevel(hwnd, 2)` gives 0. `GetSpeakerConfig` gives 0x00140004;
    4. the game's buffer (20-byte desc, `0x18000`, 21,504 bytes, PCM 2/44,100/16) is made, and the audio log line reads `audio buffer 1: 2 ch, 44100 Hz, 16 bit, 21504 bytes, flags 0x18000`;
    5. `Lock(0, 0, .., 2)` gives (data, 21504, 0, 0), and the block is all zero;
    6. after `Play(0, 0, 1)`, `GetStatus` gives 5, and the play cursor has moved within 200 ms (null sink);
    7. `Stop`, then `GetStatus` gives 0. The last `Release` of the buffer, then of the object, leaves `d3d_live_count` as before.
  - `ds_buffer_lock`: `Lock(21000, 1024)` gives n1 504, p2 = data, n2 520. `Lock(21504, 4)`, `Lock(0, 21508)` and `Lock(0, 0, .., 0)` give `DSERR_INVALIDPARAM`. `FROMWRITECURSOR` starts at the write cursor. `Unlock` with a pointer outside the buffer gives `DSERR_INVALIDPARAM`.
  - `ds_buffer_stream` (with `ds_out_manual_for_test`): the game's streaming loop in C, 200 times: read the play cursor, `Lock`/`Unlock` 512-byte chunks of a counting 16-bit pattern from the loop's own write offset up to it, then `ds_mix` 441 frames. After the first full buffer, the mixed output is the pattern in order, with no sample repeated or skipped.
  - `ds_stops` (each in a child, exit status and line checked):
    - flags `0x18014` names bits `0x14`;
    - `CoCreateInstance(CLSID_DirectSound8)`;
    - a `WAVE_FORMAT_EXTENSIBLE` format names tag 0xFFFE;
    - `Play(0, 0, 0)`;
    - `SetCooperativeLevel(hwnd, 4)`;
    - `GetCaps` and `SetVolume` (not implemented);
    - `CoCreateInstance(CLSID_DirectSoundCapture)`;
    - `DirectSoundCreate`;
    - `Initialize` with a GUID other than the two allowed;
    - `LoadLibraryA("OpenAL32.dll")` prints `reached openal32.dll: owned by a later plan` with exit 4.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_audio.py -v`. Expected: the new groups FAIL.
- [ ] **Step 3: Implement** `dsound.c`, the tables and the `later.c` changes.
- [ ] **Step 4: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_audio.py tests/test_engine.py tests/test_input.py -q`. Expected: all pass. Then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: `test_boot_to_audio` now fails, at the `dsound.dll` probe's `GetProcAddress` (a call to address 0). Task 4 answers it and Task 5 rewrites that test. So that no committed task leaves a red test, mark `test_boot_to_audio` `@pytest.mark.xfail(strict=True, reason="plan 8: the boot passes DirectSound; Task 5 rewrites this test")` in this step and confirm it reports `XFAIL`. Task 5 removes the mark when it rewrites the test. (Under Task 4 it stays `XFAIL`: the boot no longer stops at audio.)
- [ ] **Step 5: Commit.** `git commit -m "Add IDirectSound and its streaming buffers"`

### Task 4: dsound.dll's device enumeration and the waveOut guard

**Files:**
- Modify: `runtime/bw2/audio/dsound.c`, `runtime/bw2/d3d9/d3d9_methods.c`, `runtime/bw2/audio/audio_selftest.c`, `tests/test_audio.py`.

**Interfaces:**
- Consumes: win32hle's system module for `dsound.dll` (`LoadLibraryA` gives it, as it is in win32hle's system list) and `GetProcAddress`, which answers a system module's names from the shim registry. Also `hle_call_guest`.
- Produces:
  - Shim `DllGetClassObject(rclsid, riid, ppv)`, registered by `bw2_audio_register`:
    - `CLSID_DirectSoundPrivate` with `IID_IClassFactory` gives `S_OK` and a class factory;
    - another class or IID stops loudly, naming the GUID (`dsound: DllGetClassObject of class {...} not answered`).
    - The shim goes into the global shim registry, and win32hle's `GetProcAddress` answers every system module by name from that registry. So `GetProcAddress` of `ole32.dll` or any other system module for `"DllGetClassObject"` also gets this shim. That is acceptable because the shim answers only `CLSID_DirectSoundPrivate` and stops loudly, naming the class, for anything else; nothing is answered wrongly in silence.
  - `IClassFactory` (private), IID {00000001-0000-0000-C000-000000000046}. Methods: `QueryInterface`, `AddRef`, `Release`, `CreateInstance`, `LockServer`.
    - `CreateInstance(outer, riid, ppv)`: outer must be NULL (`CLASS_E_NOAGGREGATION` 0x80040110), and riid `IID_IKsPropertySet` gives the property set. Another IID gives `E_NOINTERFACE`.
    - `LockServer` gives `S_OK`.
  - `IKsPropertySet`, IID {31EFAC30-515C-11D0-A9AA-00AA0061BE93}. Methods: `QueryInterface`, `AddRef`, `Release`, `Get`, `Set`, `QuerySupport`. `Set` and `QuerySupport` stop as not implemented.
  - **`Get(set, id, inst, cbinst, data, cbdata, &ret)`:** only `DSPROPSETID_DirectSoundDevice` {84624F82-25EC-11D1-A4D8-00C04FC28ACA} with id 7 (enumerate, narrow strings), cbdata >= 8. It calls the guest `callback(desc, context)` (stdcall, `hle_call_guest`) once, for the one render device, then sets *ret = 8 and gives `S_OK`. Any other set or id stops loudly, naming both.
  - **The device description** (40 bytes in guest memory, laid out as the game reads it, "Measured" item 3). The three string pointers (+0x18, +0x1C, +0x20) each point to a valid NUL-terminated string in guest memory (`hle_alloc`'d once, kept for the run); the game copies the +0x20 string without checking the pointer:
    - +0 Type 2 (`WDM`), +4 DataFlow 0 (render);
    - +8 DeviceId {B5D2D6A1-6E0B-4C1F-9A3E-0DA5B0C0A801}, a fixed GUID of my choosing for the Mac's default output. My call;
    - +0x18 Description `"Default output"`, +0x1C Module `"coreaudio"`;
    - +0x20 Interface: a nonzero pointer to a one-byte string holding only the NUL, `""` (my call: no `\\?\` device path, so the game's acceleration lookup finds nothing and it keeps its default, the DirectSound output; Ruling 3);
    - +0x24 WaveDeviceId 0.
  - **The waveOut guard:** a BW2 `waveOutOpen` shim, registered before win32hle's, stops loudly through `d3d_fail("waveOutOpen: the waveOut output is not answered")` (the address comes from `d3d_fail`'s own suffix). win32hle's silent device would let the game's other output run with no sound, which is silent wrongness. `waveOutGetNumDevs`, `waveOutGetDevCapsA` and the rest stay win32hle's.

- [ ] **Step 1: Write the failing tests** (groups in `audio_selftest`):
  - `ds_private`, the probe's sequence ("Measured" item 3) with the game's GUIDs:
    - `LoadLibraryA("dsound.dll")` gives a nonzero module, and `GetProcAddress(it, "DllGetClassObject")` gives a nonzero entry;
    - the call gives `S_OK` and a factory. `CreateInstance(NULL, IID_IKsPropertySet)` gives a property set;
    - `Get(.., 7, NULL, 0, data, 8, &ret)` calls a selftest callback (a guest-callable shim, registered as the shims selftest's `selftest_completion` is) exactly once. It sees Type 2, DataFlow 0, the DeviceId above, the two strings (pointers nonzero), an Interface pointer that is nonzero and points to a byte 0, WaveDeviceId 0 and the context passed through. ret is 8;
    - `Release` of the set and the factory brings `d3d_live_count` back.
  - `ds_private_rules`: `CreateInstance` with an outer gives 0x80040110. In children, each of these stops with its line: `DllGetClassObject(CLSID_DirectSound, IID_IClassFactory)` (naming the class), `GetProcAddress(LoadLibraryA("ole32.dll"), "DllGetClassObject")` called with `CLSID_DirectSound` (the same shim, the same stop naming the class), `Get` with id 8 (naming the id), `Get` with another set (naming the set), and `waveOutOpen` (the bare `waveOutOpen: the waveOut output is not answered` text, with the guest address appearing once).
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_audio.py -k private -v`. Expected: FAIL.
- [ ] **Step 3: Implement** the private objects, the shim and the guard.
- [ ] **Step 4: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_audio.py -q`. Expected: all pass. Then `BW2_HEADLESS=1 BW2_AUDIO_LOG=1 BW2_SUPPORT=$(mktemp -d) timeout 20 out/bw2 --game gamefiles/game > .superpowers/sdd/plan8-boot.txt 2>&1; tail -20 .superpowers/sdd/plan8-boot.txt`. Expected: no loud line, the buffer line from "Measured" item 4, audio second lines, and the DirectInput notice. The run ends at the timeout (exit 124).
- [ ] **Step 5: Commit.** `git commit -m "Answer dsound.dll's device enumeration"`

### Task 5: The boot to the front end, measured

**Files:**
- Create: `docs/research/frontend.md`.
- Modify:
  - `runtime/bw2/input/dinput8.c`: `BW2_QUIT_AFTER_READS`;
  - `runtime/bw2/input/input_selftest.c`, `tests/test_input.py`;
  - `tests/test_boot.py`: `test_boot_to_audio` becomes `test_boot_to_front_end` (Task 3's `xfail` mark is removed), and the module docstring;
  - the spec: section 1's opening sentence, section 5.1's OpenAL line and its "Left to the builder" OpenAL line, and section 5.2 (Audio paragraph, and a plan 8 note in the Files paragraph about the `OPEN_ALWAYS` rule);
  - `docs/superpowers/plans/2026-11-11-bw2-plan6-input.md`: the status note.

**Interfaces:**
- Consumes: everything above.
- Produces:
  - **`BW2_QUIT_AFTER_READS=<n>`** (read once, n >= 1): at the nth device read (`GetDeviceData` or `GetDeviceState`, counted together), dinput8.c prints `[bw2] quit after <n> input reads (BW2_QUIT_AFTER_READS)` and calls `hle_exit(0)` before answering it. Unset, nothing changes. A value that is not a positive integer stops the run at the first read, naming it.
  - The measured record plan 6 starts from.

- [ ] **Step 1: Write the failing tests.**
  - `di_quit` (`tests/test_input.py`, in a child): with `BW2_QUIT_AFTER_READS=3`, the third `GetDeviceData` exits 0 with the quit line, and the first two answer as before. With `BW2_QUIT_AFTER_READS=x`, the first read stops naming `x`.
  - `test_boot_to_front_end` (`BW2_GAMEFILES=1`), run with `BW2_FRAME_LOG=1`, `BW2_FONT_LOG=1`, `BW2_AUDIO_LOG=1`, `BW2_QUIT_AFTER_READS=300` and a fresh `BW2_SUPPORT`. It asserts:
    - exit code 0 and the quit line for 300;
    - no line in `LOUD`, and no `owned by a later plan` line;
    - the first 49 font lines match the 49 that `test_boot_to_audio` checks today (`fonts[:49]`), and the script plugin's line is present. The front end may make more fonts; the total is not asserted unless Step 4 measures it stable over its runs;
    - exactly one audio buffer line, `audio buffer 1: 2 ch, 44100 Hz, 16 bit, 21504 bytes, flags 0x18000`;
    - at least one `audio second` line with nonsilent above 0;
    - the DirectInput notice exactly once;
    - at least 300 frame lines, numbered from 1, and at least one frame after the first input read at least half non-black (what the menu shows in its last frame was not measured, so the last frame is not asserted unless Step 4 measures it);
    - no file under `<support>/Game/` (Task 1's rule: the banks are read in place).
- [ ] **Step 2: Run them.** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_input.py -k quit -v && BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement `BW2_QUIT_AFTER_READS`.**
- [ ] **Step 4: Bring-up loop.** Run `BW2_HEADLESS=1 BW2_AUDIO_LOG=1 BW2_FRAME_LOG=1 BW2_QUIT_AFTER_READS=300 BW2_SUPPORT=$(mktemp -d) out/bw2 --game gamefiles/game`, output in a file, reading its tail.
  - Each loud stop in this plan's layers (DirectSound, the mixer, Task 1's answers) is fixed with a selftest check first, red then green.
  - An unresolved call to a function start the catalog lacks is added as a `"boot"` start (Task 1, Step 5), with a full pipeline run.
  - A stop at Bink, OpenAL or another later plan's name before the front end is recorded, and the boot test's expected end changes to that stop.
  - Each stop outside every plan's layer and each tripwire stops the run and goes to the user, as does any disc, volume or driver query from the protection VM.
  - Before fixing the boot test's assertions, measure at 300 reads over two runs (one with a fresh support folder): the font line count, the non-black share of the last frame and of each frame after the first read, and whether any game debug line (the model loader's 12 not-found lines included) contains a `LOUD` substring. Tighten the font total or the last-frame check only if both runs agree. If a game line matches `LOUD`, narrow the check so that game line no longer matches while every engine stop format still does (test sources never contain the game's strings, so the fix keys on the engine's own line shapes), and say so in `frontend.md`.
- [ ] **Step 5: Measure and write `docs/research/frontend.md`.** It holds facts only (addresses, sizes, counts, GUIDs, and DLL, API and file names). It never quotes the game's own output: a debug line is described in neutral words with the address that prints it, as "Measured" does. It records:
  - the time from start to the first input read and to the first non-silent audio second (four runs, two with a fresh support folder), and the frame numbers;
  - the audio sequence of "Measured" items 1-5 as it runs now: each DirectSound and private call with its count over a 60 s run, the buffer line, and the audio seconds (how many are non-silent, and the peak range);
  - the output choice and the probe (items 2-3), and why the DirectSound output is taken;
  - the OpenAL loader and the waveOut output, both unused, and what would reach them;
  - Task 1's fixes, with their addresses and counts (59 opens, the 78 `GetKeyNameTextW` calls and the measured count of `GetKeyNameTextA` fallback calls after an empty W answer, at least 38, with their call sites from "Measured" item 9, the start `0x8B00B0`);
  - the bad catalog entry `sub_008B00D6` ("Measured" item 7), for later catalog work, with a trace-confirmed statement that no run dispatches to `0x8B00D6`;
  - the front end: frames per second headless, the reads per frame, the message pump, and the imports seen in 60 s after the first read that are not seen before it. Also the absence of `GetCursorPos`, `GetAsyncKeyState`, `GetKeyState`, Bink and any disc or volume question;
  - the model loader's 12 not-found lines, counted, with the address that prints them;
  - for plan 6: where the reads come from (`sub_009AFD20`, the mode switch `sub_009AF790`), and that the front end now waits for input.
- [ ] **Step 6: Update the docs.**
  - **Spec 5.2 Audio:** the game mixes in software and streams one DirectSound buffer, which a CoreAudio unit plays. The probe's private enumeration is answered, OpenAL and waveOut are unused, and EAX is not needed on this path.
  - **Spec section 1 and 5.1:** the opening sentence's "DirectInput, OpenAL and Bink" becomes "DirectInput, DirectSound and Bink", and 5.1's "OpenAL + EAX on macOS" becomes "DirectSound on CoreAudio (the game mixes in software; OpenAL + EAX only if a later boot loads `wrap_oal.dll`)". The "Left to the builder" line on the OpenAL implementation gets a pointer to Ruling 2.
  - **Spec 5.2 Files:** a read-only open of an existing game file never touches the overlay.
  - **Plan 6's status note:** the game now reads its mouse at the front end, once per frame. Plan 6 is next. Before it runs, its "Measured" section is redone from `frontend.md` and its Task 2 is rebased on plan 5's stub (`runtime/bw2/input/dinput8.c`), keeping `BW2_QUIT_AFTER_READS`.

  Then run the full suite: `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan8-suite.txt 2>&1; tail -3 .superpowers/sdd/plan8-suite.txt`. Expected: all pass.
- [ ] **Step 7: Audit and commit.** `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through audio to the front end"`

## Rulings (my calls; the spec leaves them to the builder)

1. **Scope and phasing:** plan 8 answers audio and the three things between audio and the front end (a file-open rule, key names, one missed start). Input stays plan 6's: the front end is where the game first reads input, so plan 6 is the next plan, rewritten from `frontend.md`.
2. **DirectSound, not OpenAL.** The game's audio library mixes in software and streams into one DirectSound buffer; its OpenAL loader never runs in the default configuration, and the discs ship no OpenAL. So the spec's "OpenAL implementation" choice is answered by not needing one. `openal32.dll` and `wrap_oal.dll` stay loud later-plan stops, for the first plan whose boot loads them (perhaps from a settings choice). No EAX is needed on this path.
3. **The device the probe sees:** one render device, type WDM, with a fixed DeviceId and no `\\?\` interface path. With no path the game's acceleration lookup reads no registry key and keeps its default. The default is the DirectSound output, which a Windows XP machine with default acceleration also takes. This also keeps the game away from device paths, which the tripwire guards.
4. **Speakers:** always stereo with the wide geometry (0x00140004), Windows' desktop default. The game's mix is stereo; CoreAudio handles the device's own channel layout. Surround (`WAVEFORMATEXTENSIBLE`) stops loudly until a speaker setting asks for it.
5. **Output:** CoreAudio's default-output `AudioUnit` at 44,100 Hz float stereo. AudioToolbox and CoreAudio are Apple system frameworks, so there is no third-party licence (spec 9). Headless, and when the unit cannot start, a real-time null sink keeps the game's cursors moving. A unit that cannot start prints one notice line.
6. **Cursors:** the play cursor is exactly what the mixer has consumed; the write cursor leads it by 512 frames. The game reads only the play cursor and writes up to it ("Measured" item 4).
7. **What the buffer supports:** PCM 8 or 16 bit, 1 or 2 channels, any rate (linear interpolation), and looping play. These are the measured buffer plus the cheap generalizations the mixer gets for free. Volume, pan and frequency control, 3D, hardware buffers, the primary buffer, one-shot play and notifications stop loudly until a call asks.
8. **The waveOut output stops loudly.** win32hle's silent waveOut would play the game's other output with no sound. waveIn stays win32hle's "no recording device", as on a PC without one.
9. **Read-only `OPEN_ALWAYS` opens in place** (vendored win32hle). It is Windows' behaviour and every title needs it. The overlay keeps getting writes and creates only. No support folder from an earlier run holds stale empty banks: no boot before this plan reached the bank opens.
10. **Key names** follow Windows' US English layout (the USA release, spec 11), for both A and W, in vendored win32hle. Other layouts are a non-goal.
11. **The missed start `0x8B00B0`** is recorded as a `"boot"` start. Discovery is not widened: of the 91 vector-iterator callbacks it is the only one missing, and a wider "after `ret`" rule finds mostly exception funclets.
12. **`BW2_QUIT_AFTER_READS`** ends a test run at the nth device read, with exit 0. Once the boot no longer stops by itself, a frame count is too dependent on loading speed (the front end appeared at frame 794 in one run and 1,065 in another). The first read marks the same point in every run.
13. **`BW2_AUDIO_LOG`** prints numbers only (per mixed second: voices, peak, non-silent frames), and no audio is ever written to disk. Printing from the CoreAudio render thread is accepted for this debug switch only.
14. **The vendored `win32hle/dsound.c` is not used.** It is an MIT DirectSound with a software mixer, which spec 11 says to build on where it fits, but it does not fit here: it needs SDL2, which the headless core excludes; it has a fixed table (`MAX_BUFS` 256), against spec 7.2's no fixed-size tables; and it answers unknown calls silently. It stays out of `WIN32HLE_CORE`, and `runtime/bw2/absent.c`'s empty `win32hle_dsound` table and its `hle_dsound_register_com` (an `ABSENT` stop, never called) stay unchanged. BW2's DirectSound lives in `runtime/bw2/audio/` on plan 4's `d3d_com`.
15. **Exit and the output.** `hle_exit` is `_exit` on macOS, so the process goes with no `atexit` or destructor race against the render callback. The `atexit` stop covers only `exit()` paths, and `hle_on_exit` stays `main.c`'s.
16. **The null sink's catch-up is capped** at 2,048 frames per wake, with any larger backlog dropped, so a stall of the sink's own thread never laps the game's buffer. A lap from the game's own stall is accepted as DirectSound behaviour.
