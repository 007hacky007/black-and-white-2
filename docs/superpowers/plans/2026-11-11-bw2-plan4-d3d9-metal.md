# Plan 4: Direct3D 9 on Metal (fixed function) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game gets past `Direct3DCreate9`. It finds one adapter, creates a device, and renders through Metal into a Mac window, or into an offscreen target when headless. Fixed-function rendering, resources and states follow D3D9 semantics, and each is proven by pixel tests. DirectDraw answers the video memory question, and D3DX's math and texture functions run natively. The boot then stops at the first call into plan 5's layer: programmable shaders, effects, sprite, font or mesh.

**Architecture:**
- Each D3D9 object is a guest COM object built with win32hle's `hle_com_vtable` / `hle_com_new`. The guest object holds the vtable, a reference count and a host id. The id indexes a growable table of host structs, so there are no fixed-size tables.
- Methods are named stdcall shims in vtable order, so dispatch is by method index. An unimplemented method of this plan's layer stops loudly, naming it. A method owned by plan 5 exits 4, like `later.c`.
- The Metal side is Objective-C with ARC under `runtime/bw2/d3d9/`. Fixed-function state becomes a key, the key becomes generated MSL, and the MSL becomes a pipeline state object. Both are cached in memory, with a Metal binary archive on disk.
- The guest runs on its own big-stack thread, as in plan 3. The process main thread now runs the run loop, so window work can be sent to it.

**Tech Stack:** C and Objective-C (clang, arm64, ARC, macOS 13+), Metal, QuartzCore (`CAMetalLayer`), AppKit, ImageIO (PNG/JPEG/BMP decode and encode), pcrecomp win32hle and recomp32 (vendored, MIT), Python 3.14 and pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 5.2 D3D9 and D3DX, 7.1 x87 precision, 7.2 no fixed tables, 7.3 Direct3D 9). Facts: `docs/research/d3dx-usage.md` (the 60 D3DX imports and their callers), `docs/research/boot.md` (where plan 3 stops).

## Measured before writing (2026-11-11)

- **Imports:**
  - d3d9 imports only `Direct3DCreate9` (IAT `0xC12580`).
  - DDRAW imports `DirectDrawEnumerateA` and `DirectDrawCreateEx`.
  - D3DX imports 60 functions, every one of them called (`d3dx-usage.md`).
- **DirectDraw use:**
  - `0x934F30`: `CoInitialize(0)`, then `CoCreateInstance(CLSID_DirectDraw, 0, CLSCTX_ALL=7, IID_IDirectDraw7, &dd)`, then `Initialize(NULL)` (slot 18), then `GetAvailableVidMem({DDSCAPS_LOCALVIDMEM 0x10000000}, &total, &free)` (slot 23).
  - `0x5F0BB0`: `DirectDrawCreateEx(guid, &dd, IID_IDirectDraw7, 0)` through `0xAF3A56`, then `GetDeviceIdentifier(&id, 0)` (slot 27), then `Release`. It formats the driver version into a string.
  - Nothing else is called on IDirectDraw7.
- **What the game reads from D3D9 (`0x933B70`):**
  - `IDirect3DDevice9::GetAvailableTextureMem` (slot 4).
  - `GetDeviceCaps` (slot 7). It compares `PixelShaderVersion` (caps offset `0xCC`) with `0xFFFF0200`.
  - The adapter identifier: it compares `VendorId` with `0x1002` (ATI). It never compares with NVIDIA's `0x10DE`.
  - When the vendor is not ATI, a flag comes from an OS version check (`0x932BE0`, `GetVersionExA`).
- **x87 precision:** recomp32 keeps x87 as doubles and reads `g_fpu_cw` only for rounding (`fp_to_int`). Precision control (bits 8-9) is ignored. The game has 2,594 `fldcw` sites. Spec 7.1 requires the 24-bit mode that D3D9's `CreateDevice` sets.
- **Threads:** `runtime/bw2/threads.c` runs the guest on a 1 GB pthread, and the process main thread sits in `pthread_join`. AppKit needs the main thread.
- **Test harness:** `test_shims_selftest` compiles the engine sources minus `main.c`, plus a C selftest, and calls shims as lifted code does (`shims_selftest.c` `call()`). This plan's D3D9 tests use the same pattern, with a COM `vcall`.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings, bytes, shaders or textures copied from them, or generated C. Facts only (addresses, sizes, hashes). Run `third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data. Test images and vertex data are made by the tests themselves.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The plan 3 tripwire stays on, and a tripped run is reported, never worked around.
- Fail loudly, never return 0 silently:
  - An unimplemented D3D9, DirectDraw or D3DX method in this plan's layer stops the run with `d3d9: <Interface>::<Method> not implemented (from guest 0x%08X)`, through `bw2_fail`.
  - An unsupported state value stops the run naming the state and the value: vertex blending, point sprites, N-patches, stream frequency other than 1, `ProcessVertices`, patches, MSAA quality above 0.
- Plan 5 owns these, and each exits 4 with `[bw2] reached <name>: owned by a later plan`:
  - device `CreateVertexShader` and `CreatePixelShader`;
  - D3DX `D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXAssembleShader`, `D3DXCreateEffect`, `D3DXCreateEffectPool`, `D3DXCreateFragmentLinker`, `D3DXCreateSprite`, `D3DXCreateFontA`, `D3DXCreateSphere`, `D3DXGetShaderVersion`, `D3DXGetVertexShaderProfile`, `D3DXGetPixelShaderProfile`.

  DirectInput, DirectSound/OpenAL and Bink stay as in plan 3.
- Exit codes are unchanged from plan 3: 0 normal, 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- **The device the guest sees (my calls; the user may override):**
  - One adapter. Identifier:
    - Driver `bw2metal.dll`; Description `BW2 virtual GPU (Metal)`.
    - VendorId `0x106B`, DeviceId `0x0001`, SubSysId 0, Revision 0.
    - DriverVersion 6.14.10.0; WHQL level 1.
    - DeviceIdentifier GUID `{B2B2B2B2-0000-4000-8000-000000000001}`.
    - Not ATI, so the game takes its non-ATI path.
  - Caps are those of an SM 3.0 part (exact values in Task 3).
  - Video and texture memory: 512 MB total, 512 MB free (`GetAvailableVidMem`, `GetAvailableTextureMem`).
  - Display: the Mac's main display in pixels, 60 Hz, `D3DFMT_X8R8G8B8`. In headless mode it is fixed at 1920x1080 for reproducible tests.
- **Headless mode:** `BW2_HEADLESS=1` means no NSApplication and no window. The back buffer is presented to nowhere, and the frame counter still advances. Every test runs headless.
- `CreateDevice` without `D3DCREATE_FPU_PRESERVE` (0x2) sets the calling thread's x87 precision to 24-bit and its rounding to nearest, as Wine's d3d9 does. With the flag set it changes nothing.
- Memory is based (`MEM_BASE` 0x100000000000ull) for every file. Objective-C files compile with `-fobjc-arc`. The engine links `-framework Metal -framework QuartzCore -framework AppKit -framework Foundation -framework ImageIO -framework CoreGraphics`.
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES` (file - what - why). No NFSU2Recomp code: the D3D9 layer is written from the D3D9 documentation.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Identity is 007hacky007. Never push. Work on branch `plan4`.

## Review Focus

- **A dynamic vertex buffer locked with `D3DLOCK_DISCARD` before each of many draws in one frame:** every draw shows its own vertices, never a later lock's. (Task 6, `dynamic_discard`.)
- **A back buffer smaller or larger than the window's drawable:** Present scales it to fit, and reading back the back buffer still gives back-buffer-sized pixels. (Task 4, `present_scaled`.)
- **A texture released by the game while still set on a stage:** the device's binding keeps it alive until it is unbound, as D3D9's own reference does, with no use-after-free. (Task 5, `release_while_bound`.)
- **Tiny mips of compressed textures** (DXT1 2x2 and 1x1 levels, non-power-of-two bases): the pitch, the lock size and the upload are right at every level. (Task 5, `dxt_small_mips`.)
- **`Reset` while `D3DPOOL_DEFAULT` resources still exist:** it fails with `D3DERR_INVALIDCALL` as documented, and the device stays usable. After they are released, `Reset` succeeds and restores the default states. (Task 4, `reset_rules`.)

---

### Task 1: Objective-C in the engine, and a main thread that serves the window

**Files:**
- Modify: `bw2/build.py` (`engine_sources()` also returns `runtime/bw2/d3d9/*.c` and `runtime/bw2/d3d9/*.m`, minus `*_selftest.*`; `.m` files compile with `-fobjc-arc`; engine `HOST_LIBS` gain the frameworks in Global Constraints)
- Create: `runtime/bw2/d3d9/mac_main.m`
- Modify: `runtime/bw2/threads.c` (the main thread runs `bw2_mac_main_loop` instead of `pthread_join`), `runtime/bw2/bw2.h`
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces:
  - `void bw2_main_sync(void (^block)(void))`: runs `block` on the process main thread and waits. If already on it, the block runs inline.
  - `int bw2_headless(void)`: 1 when `BW2_HEADLESS` is `1`.
  - `void bw2_mac_main_loop(pthread_t guest)`: returns, with the guest's exit status, once the guest thread ends.
    - Headless: it runs `CFRunLoopRun` in slices until the guest ends.
    - Otherwise it sets up `NSApplication` (activation policy regular, `finishLaunching`, a minimal app menu with Quit) and pumps events until then.
  - Engine flag `--selftest-mainsync`: from the guest thread, `bw2_main_sync` runs a block that checks `pthread_main_np()`. It prints `main sync ok` and exits 0.

- [ ] **Step 1: Write the failing test** `test_main_sync_runs_on_main_thread`: a `ret` guest built with `_engine`, run with `--selftest-mainsync` and `BW2_HEADLESS=1`. Assert `returncode == 0` and `"main sync ok" in stdout`.
- [ ] **Step 2: Run it.** `.venv/bin/pytest tests/test_engine.py::test_main_sync_runs_on_main_thread -v`. Expected: FAIL (unknown flag, or a link error for `bw2_main_sync`).
- [ ] **Step 3: Implement** the build changes, `mac_main.m`, the threads.c change and the flag in `main.c`.
- [ ] **Step 4: Run it, then the engine suite.** `.venv/bin/pytest tests/test_engine.py -q`. Expected: all pass, including the plan 3 exit-code tests (an exit from the guest thread still ends the process with its code).
- [ ] **Step 5: Commit.** `git commit -m "Run the main thread's run loop beside the guest"`

### Task 2: x87 precision control

**Files:**
- Modify: `third_party/pcrecomp/runtime/recomp32/recomp_types.h` (`FP_PC(x)`), `third_party/pcrecomp/tools/lift/lift32.py` (wrap the results of `fadd`/`fsub`/`fsubr`/`fmul`/`fdiv`/`fdivr`, all their forms including `fi*` and `*p`, and `fsqrt` in `FP_PC`), `third_party/pcrecomp.PATCHES`
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces: `FP_PC(x)`, which is `((g_fpu_cw & 0x300) == 0 ? (double)(float)(x) : (x))`. Only arithmetic results are rounded: loads, stores and compares are unchanged, as on x87.

- [ ] **Step 1: Write the failing test** `test_x87_single_precision_mode`. The guest does the following:
  - `fldcw` 0x007F, then computes `fld1; fld dword [three]; fdivp`, then `fstp qword [out1]`;
  - `fldcw` 0x027F, then the same into `out2`;
  - prints both as hex through the PUTS helper.

  Assert `out1 == struct.pack('<d', struct.unpack('<f', struct.pack('<f', 1/3))[0])` and `out2 == struct.pack('<d', 1/3)`.
- [ ] **Step 2: Run it.** Expected: FAIL, because `out1` is the double 1/3.
- [ ] **Step 3: Implement** `FP_PC` and the lifter change. Add the PATCHES line: `recomp_types.h, lift32.py - x87 precision control (PC=24 rounds arithmetic results to float) - D3D9 CreateDevice sets single precision and BW2's math expects it`.
- [ ] **Step 4: Run it, then `.venv/bin/pytest tests/test_lift.py tests/test_engine.py -q`.** Expected: all pass.
- [ ] **Step 5: Relift and measure.** `python -m bw2.pipeline` (full lift and build). Record the change in engine size and build time in the ledger. Expected: it builds; `tests/test_boot.py -q` passes.
- [ ] **Step 6: Commit** in two commits: `git commit -m "Honour x87 precision control"` (the vendored files and PATCHES), then the test as `git commit -m "Test x87 single precision"`.

### Task 3: COM objects, IDirect3D9 and DirectDraw 7

**Files:**
- Create:
  - `runtime/bw2/d3d9/d3d_com.c`, `d3d_com.h`: the object table and the method tables;
  - `runtime/bw2/d3d9/d3d9_methods.c`: every interface's method names in vtable order, from the D3D9 headers' declaration order;
  - `runtime/bw2/d3d9/d3d9_adapter.c`: IDirect3D9;
  - `runtime/bw2/d3d9/ddraw7.c`;
  - `runtime/bw2/d3d9/d3d9_selftest.c`.
- Modify: `runtime/bw2/register.c` (register `Direct3DCreate9`, `DirectDrawCreateEx`, `DirectDrawEnumerateA`, and the CLSID_DirectDraw class via `hle_com_register_class`), `runtime/bw2/later.c` (drop `d3d9.dll`, `ddraw.dll` and their names; add a stop for each D3DX name in Global Constraints, while `d3dx9_25.dll`'s other imports are answered by Tasks 8 and 9)
- Test: `tests/test_d3d9.py` (new). A module-scoped fixture compiles `d3d9_selftest` like `test_shims_selftest`, plus the frameworks. Each test runs `d3d9_selftest <group>` with `BW2_HEADLESS=1` and asserts exit 0 and `"<group>: 0 failed"`.

**Interfaces:**
- Produces:
  - `uint32_t d3d_new(enum d3d_kind kind, uint32_t vtable, void *host)`: a guest object `[vtable][refcount=1][host id]`.
  - `void *d3d_host(uint32_t obj, enum d3d_kind kind)`: stops loudly on a wrong kind or a dead object.
  - `void d3d_release_host(uint32_t obj)`: frees the id slot and the guest object.
  - `uint32_t d3d_vtable(const char *iface)`: built once from `d3d9_methods.c`. Each unimplemented method stops loudly, naming itself. An implementation registers itself with `d3d_impl(const char *iface_method, void (*fn)(void))` before the vtable is built.
  - The selftest helper `uint32_t vcall(uint32_t obj, int slot, int n, ...)` calls through the guest vtable and checks the stdcall pop, like `shims_selftest.c`.
- **IDirect3D9:**
  - `GetAdapterCount` = 1.
  - `GetAdapterIdentifier` as in Global Constraints.
  - `GetAdapterModeCount` / `EnumAdapterModes` for `X8R8G8B8` and `R5G6B5`. The modes are 640x480, 800x600, 1024x768, 1152x864, 1280x720, 1280x800, 1280x960, 1280x1024, 1360x768, 1440x900, 1600x900, 1600x1200, 1680x1050, 1920x1080, 1920x1200, 2560x1440, 2560x1600 and the display's own size. Those larger than the display are dropped, duplicates are merged, the list is sorted by width then height, and the refresh rate is 60. Other formats have 0 modes.
  - `GetAdapterDisplayMode`.
  - `CheckDeviceType`: HAL; windowed with any of the two display formats; full screen when the back buffer format matches.
  - `CheckDeviceFormat` from Task 5's format table and its usage flags.
  - `CheckDeviceMultiSampleType`: only `D3DMULTISAMPLE_NONE`, quality levels 1.
  - `CheckDepthStencilMatch`: any table depth format with any table render target format.
  - `CheckDeviceFormatConversion`: X8R8G8B8 and A8R8G8B8 to each other.
  - `GetDeviceCaps`.
  - `GetAdapterMonitor` = 0x4D4F4E31.
  - `CreateDevice` (Task 4).
  - `RegisterSoftwareDevice` = `D3DERR_INVALIDCALL`.
  - Adapter numbers other than 0 give `D3DERR_INVALIDCALL`, and `D3DDEVTYPE_REF`/`SW` give `D3DERR_NOTAVAILABLE`.
- **Caps (the device's too):**
  - `DeviceType` HAL.
  - `VertexShaderVersion` 0xFFFE0300, `PixelShaderVersion` 0xFFFF0300, `MaxVertexShaderConst` 256, `PixelShader1xMaxValue` 65504.
  - `MaxTextureWidth` and `MaxTextureHeight` 4096; `MaxVolumeExtent` 512; `MaxTextureAspectRatio` 4096; `MaxTextureRepeat` 8192; `MaxAnisotropy` 16.
  - `MaxTextureBlendStages` 8, `MaxSimultaneousTextures` 8, `MaxActiveLights` 8, `MaxUserClipPlanes` 6, `MaxVertexBlendMatrices` 0.
  - `MaxPointSize` 1.0.
  - `MaxPrimitiveCount` 0xFFFFF, `MaxVertexIndex` 0xFFFFFF, `MaxStreams` 16, `MaxStreamStride` 2048.
  - `NumSimultaneousRTs` 4.
  - `DevCaps` with HWTRANSFORMANDLIGHT, HWRASTERIZATION, DRAWPRIMITIVES2EX, TEXTUREVIDEOMEMORY, PUREDEVICE.
  - TextureCaps: no POW2 and no NONPOW2CONDITIONAL; with PERSPECTIVE, ALPHA, MIPMAP, CUBEMAP, VOLUMEMAP, MIPCUBEMAP, MIPVOLUMEMAP, PROJECTED.
  - Every blend, compare, stencil, address and filter cap that Task 6 implements, and no others.
  - The vs/ps 2.0 extended caps (`VS20Caps`, `PS20Caps`) at their maximums.
  - `MaxVShaderInstructionsExecuted` and `MaxPShaderInstructionsExecuted` 65535; 3.0 instruction slots 32768.
  - `DeclTypes`: UBYTE4, UBYTE4N, SHORT2N, SHORT4N, USHORT2N, USHORT4N, FLOAT16_2, FLOAT16_4.
- **IDirectDraw7:**
  - `QueryInterface`, `AddRef`, `Release`.
  - `Initialize(NULL)` = DD_OK.
  - `GetAvailableVidMem` = 512 MB total and free, for any caps.
  - `GetDeviceIdentifier` gives the adapter identifier, in DirectDraw's layout (512+512-byte strings).
  - `GetCaps` gives video memory in `dwVidMemTotal`/`dwVidMemFree`.
  - `DirectDrawEnumerateA` calls back once: GUID NULL, "Primary Display Driver", "display", via `hle_call_guest`.

- [ ] **Step 1: Write the failing tests** in `d3d9_selftest.c` group `adapter` and `tests/test_d3d9.py::test_adapter`. Check:
  - every IDirect3D9 value above, including the mode list in headless mode, which ends with 1920x1080 and has no 1920x1200;
  - `CheckDeviceFormat` of DXT1 for a texture is OK, and `D16_LOCKABLE` is `D3DERR_NOTAVAILABLE`;
  - the vtable is 17 slots, with `CreateDevice` at slot 16.

  Group `ddraw`, `test_ddraw`: the CoCreateInstance sequence from "Measured" gives 512 MB, and `DirectDrawCreateEx` + `GetDeviceIdentifier` gives VendorId 0x106B. Group `objects`, `test_many_objects`: 200,000 objects created, every other one released, all live ones resolve, and none are mixed up. Group `unimplemented`, `test_unimplemented_method_is_loud`: calling a slot with no implementation (IDirect3DDevice9 `DrawRectPatch`, through a device stub object) exits non-zero with `IDirect3DDevice9::DrawRectPatch not implemented` in stderr.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_d3d9.py -v`. Expected: FAIL (the selftest does not build: no `d3d_new`).
- [ ] **Step 3: Implement** the files above.
- [ ] **Step 4: Run them.** Expected: PASS. Then `.venv/bin/pytest tests/test_engine.py -q`: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Add IDirect3D9 and DirectDraw 7"`

### Task 4: Device, swap chain and Present on Metal

**Files:**
- Create:
  - `runtime/bw2/d3d9/mtl_device.m` and `mtl.h`: MTLDevice, command queue, frame command buffers, the window and `CAMetalLayer`, present;
  - `runtime/bw2/d3d9/d3d9_device.c`: device methods that are not resources or drawing;
  - `runtime/bw2/d3d9/d3d9_state.c`: every render, sampler and texture stage state, with the D3D9 documented defaults, plus Get for every Set.
- Test: `tests/test_d3d9.py`, groups in `d3d9_selftest.c`

**Interfaces:**
- **`CreateDevice`:**
  - Validates its arguments. `BackBufferCount` is 0 or 1. A width or height of 0 in windowed mode takes the window's client size, from win32hle `GetClientRect`. Formats come from the mode list, or `D3DFMT_UNKNOWN` when windowed.
  - Full-screen parameters are accepted and shown as a window of the back buffer size. The display plan does real full screen.
  - `EnableAutoDepthStencil` creates the depth surface.
  - Applies the FPU rule from Global Constraints.
- **The window:** on first `CreateDevice` (not headless), made through `bw2_main_sync`:
  - an NSWindow titled `Black & White 2`, with a content size in points equal to the back buffer size, centred;
  - a `CAMetalLayer` whose `drawableSize` is the content size times `backingScaleFactor`.
- **`Present`:**
  - Ends the frame's command buffer.
  - Draws the back buffer into the drawable, scaled with linear filtering to fill it while keeping the aspect ratio, with black bars.
  - Presents and starts the next frame.
  - The back buffer is never the drawable, and its contents persist after `Present` (`D3DSWAPEFFECT_DISCARD` is treated as COPY).
- **Device methods:**
  - `TestCooperativeLevel` = D3D_OK.
  - `GetAvailableTextureMem` = 512 MB.
  - `GetDirect3D`.
  - `GetDeviceCaps`.
  - `GetDisplayMode`.
  - `GetCreationParameters`.
  - `GetSwapChain(0)` / `GetNumberOfSwapChains` = 1. The implicit swap chain's own `Present`, `GetBackBuffer` and `GetPresentParameters` are implemented.
  - `GetBackBuffer`.
  - `GetRasterStatus`: InVBlank FALSE, ScanLine from time.
  - `SetDialogBoxMode`.
  - `SetGammaRamp` / `GetGammaRamp`: stored and not applied; the display plan applies it.
  - `SetCursorProperties`, `SetCursorPosition`, `ShowCursor`: stored; the input plan draws them.
  - `BeginScene` / `EndScene`, with D3DERR_INVALIDCALL on bad nesting.
  - `Clear`: flags TARGET, ZBUFFER, STENCIL, with rects or the whole viewport, clipped to the viewport and the scissor rect when scissor test is on.
  - `SetViewport` / `GetViewport`.
  - `SetScissorRect` / `GetScissorRect`.
  - `SetRenderTarget(0..3)` / `GetRenderTarget`. Setting RT0 resets the viewport to its size, as documented.
  - `SetDepthStencilSurface` / `GetDepthStencilSurface`.
  - `CreateRenderTarget` and `CreateDepthStencilSurface`: each depth surface is its own object and texture.
  - `GetRenderTargetData` into a `D3DPOOL_SYSTEMMEM` surface.
  - `Reset`:
    - fails with `D3DERR_INVALIDCALL` while any DEFAULT-pool resource made by the guest is alive (the implicit back buffer and depth surface do not count);
    - otherwise it recreates the swap chain and restores every state's default;
    - resizes the window when not headless.
  - `EvictManagedResources` = D3D_OK.
- Produces for later tasks:
  - `id<MTLTexture> mtl_rt_texture(uint32_t surface)`;
  - `void mtl_begin_pass_if_needed(void)`: opens a render pass on the current targets, loading their contents and never clearing them unasked;
  - `void mtl_set_test_drawable(uint32_t w, uint32_t h)`: headless only; Present then draws into an offscreen drawable of that size, read back by `mtl_read_test_drawable(uint32_t *rgba)`;
  - `struct d3d_state *d3d_cur(void)`: the current state struct, with dirty bits per group (blend, depth-stencil, raster, samplers, stages, transforms, lights, vertex input).

- [ ] **Step 1: Write the failing tests.** Groups and test functions in `test_d3d9.py`, one per group:
  - `clear`: a 64x64 offscreen device; `Clear` to 0xFF336699, then readback through `GetRenderTargetData` + `LockRect` gives that value at every pixel; a rect clear changes only that rect; a clear with scissor test on is limited to the scissor rect.
  - `persist`: clear, `Present`, `Present` again with nothing drawn; readback still shows the clear colour.
  - `present_scaled`: a back buffer of 320x200 presented into a 1000x1000 drawable (an offscreen `CAMetalLayer` substitute: `mtl_set_test_drawable(w, h)`). The drawable's centre pixel is the back buffer's centre colour, the top band is black, and the back buffer readback is still 320x200.
  - `reset_rules`: with a live `CreateRenderTarget` surface, `Reset` = 0x8876086C and a later `Clear` still works. After releasing it, `Reset` = 0, and `D3DRS_ZENABLE`, `D3DRS_CULLMODE` and `D3DRS_ALPHABLENDENABLE` read back their defaults after being changed.
  - `defaults`: every render state, sampler state and stage-0/stage-1 texture stage state reads back its documented default, from a table in the test that is written independently of `d3d9_state.c`.
  - `fpu`: after `CreateDevice` without FPU_PRESERVE, `g_fpu_cw & 0x300 == 0`; with it, unchanged.
  - `depth_objects`: two `CreateDepthStencilSurface` calls give different objects. Clearing one to Z=0.25 leaves the other unchanged, shown by a Z-tested clear-colour draw in Task 6; here, by `GetDepthStencilSurface` identity only.
- [ ] **Step 2: Run them.** Expected: FAIL (`CreateDevice` not implemented).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them.** `.venv/bin/pytest tests/test_d3d9.py -q`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Create D3D9 devices on Metal and present"`

### Task 5: Textures, surfaces and buffers

**Files:**
- Create: `runtime/bw2/d3d9/d3d9_format.c` (the format table), `runtime/bw2/d3d9/d3d9_resource.c` and `mtl_resource.m`
- Test: `tests/test_d3d9.py`, `d3d9_selftest.c`

**Interfaces:**
- **Format table:** D3D format, Metal storage format, swizzle, bytes per block, block size, whether the CPU converts on upload, and usage (texture, cube, volume, render target, depth stencil, vertex texture, filter, post-pixel-shader blending):
  - A8R8G8B8 to BGRA8Unorm.
  - X8R8G8B8 to BGRA8Unorm, with swizzle a=1.
  - A8B8G8R8 and X8B8G8R8 to RGBA8Unorm.
  - R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4 and X4R4G4B4 to BGRA8Unorm, converted by the CPU on upload and back on readback.
  - L8 to R8Unorm, swizzle rrr1.
  - A8 to A8Unorm.
  - A8L8 to RG8Unorm, swizzle rrrg.
  - L16 to R16Unorm, swizzle rrr1.
  - V8U8 to RG8Snorm.
  - Q8W8V8U8 to RGBA8Snorm.
  - G16R16 to RG16Unorm.
  - A16B16G16R16 to RGBA16Unorm.
  - R16F, G16R16F and A16B16G16R16F to the matching half formats.
  - R32F, G32R32F and A32B32G32R32F to the matching float formats.
  - DXT1 to BC1_RGBA.
  - DXT2 and DXT3 to BC2_RGBA.
  - DXT4 and DXT5 to BC3_RGBA.
  - D16 to Depth16Unorm.
  - D24S8, D24X8 and D24X4S4 to Depth32Float_Stencil8.
  - D32 to Depth32Float.
  - Render targets: A8R8G8B8, X8R8G8B8, R5G6B5, A16B16G16R16F, R16F, G16R16F, R32F, A32B32G32R32F.
  - Vertex textures: R32F and A32B32G32R32F.
  - Everything else is NOTAVAILABLE.
- **Objects:**
  - `CreateTexture`, `CreateCubeTexture`, `CreateVolumeTexture`, `CreateVertexBuffer`, `CreateIndexBuffer` (16- and 32-bit), `CreateOffscreenPlainSurface`.
  - Levels 0 means a full chain down to 1x1.
  - The pools are DEFAULT, MANAGED and SYSTEMMEM. SCRATCH resources have no Metal texture.
  - Usage flags: RENDERTARGET, DEPTHSTENCIL, DYNAMIC, WRITEONLY and AUTOGENMIPMAP. AUTOGENMIPMAP keeps 1 visible level; Metal generates the rest after each upload or render.
- **Locks:**
  - `LockRect`, `LockBox` and buffer `Lock` return a pointer into a guest-memory staging copy in D3D's own layout (pitch = row of blocks × bytes per block). It is allocated with `hle_alloc` on first lock and kept.
  - A partial rect lock points into it.
  - `UnlockRect` uploads the locked region, converting when the table says so.
  - `D3DLOCK_READONLY` uploads nothing.
  - `D3DLOCK_DISCARD` on a buffer gives fresh storage, so draws recorded earlier keep the old bytes.
  - `D3DLOCK_NOOVERWRITE` writes in place.
  - New resources read as zero.
- **Surfaces and other objects:**
  - `GetSurfaceLevel`, `GetCubeMapSurface` and `GetVolumeLevel` return child objects that hold a reference to their parent.
  - `GetLevelDesc`, `GetDesc`, `GetLevelCount`.
  - `SetLOD` / `GetLOD` (MANAGED only).
  - `SetPrivateData` / `GetPrivateData` / `FreePrivateData`.
  - `GetContainer`, `GetDevice`.
  - `PreLoad`, `SetPriority` / `GetPriority`.
  - `AddDirtyRect` / `AddDirtyBox`.
  - `GetDC` / `ReleaseDC` stop loudly: win32hle's `gdidc.c` needs SDL2 and is not linked. If the boot reaches them, Task 10 records it and asks for the smallest answer.
- **Copies:**
  - `UpdateSurface`, `UpdateTexture`.
  - `StretchRect`, point and linear, between render targets and textures.
  - `ColorFill`.
- **Device bindings:** `SetTexture` (stages 0-15 and the vertex samplers 257-260), `SetStreamSource`, `SetIndices` and `SetRenderTarget` add a reference that is dropped when the binding is replaced. So a released but bound resource lives until unbound.
- Produces:
  - `id<MTLTexture> mtl_texture(uint32_t obj)`;
  - `id<MTLBuffer> mtl_buffer_for_draw(uint32_t vb_or_ib, uint32_t *offset_out)`: the current storage version.

- [ ] **Step 1: Write the failing tests.** Groups, each a test function:
  - `lock_roundtrip`: for every uncompressed render-target format, lock, write a gradient, unlock, `StretchRect` into an A8R8G8B8 target, then read back. Each pixel matches the format's documented expansion, for example 5-bit 31 to 255 and 16 to 132.
  - `pitches`: for A8R8G8B8, R5G6B5, DXT1 and DXT5 textures of 256x256, 100x60 and 3x3 with full chains, check each level's `LockRect` pitch and size. DXT1 256x256 pitch is 512. DXT levels below 4x4 are one block.
  - `dxt_small_mips`: a DXT1 8x8 texture with 4 levels has each level lockable and uploadable. The 2x2 and 1x1 levels each take 8 bytes.
  - `zeroed`: a new texture, VB and IB lock as all zeros.
  - `release_while_bound`: `SetTexture(0, t)`, then `Release(t)` returns 1 (the device's reference), then `SetTexture(0, 0)` frees it. The object table no longer resolves it.
  - `child_refs`: a surface from `GetSurfaceLevel` keeps its texture alive after the texture's own release.
  - `formats_check`: `CheckDeviceFormat` agrees with the table for every format and usage.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them**, then the whole `test_d3d9.py`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Add D3D9 textures, surfaces and buffers"`

### Task 6: Draws and the fixed-function pipeline

**Files:**
- Create:
  - `runtime/bw2/d3d9/ff_key.c`: the state key;
  - `runtime/bw2/d3d9/ff_msl.c`: MSL generated from the key;
  - `runtime/bw2/d3d9/mtl_pipeline.m`: the library, pipeline and depth-stencil caches, and the binary archive;
  - `runtime/bw2/d3d9/d3d9_draw.c`: vertex input from an FVF or a declaration, and the draws.
- Test: `tests/test_d3d9.py`, `d3d9_selftest.c`

**Interfaces:**
- **Vertex input:**
  - `SetFVF`, `CreateVertexDeclaration`, `SetVertexDeclaration` and `GetVertexDeclaration`, with the declaration's `GetDeclaration`. Every `D3DDECLTYPE` in the caps is supported, and the usages are POSITION, POSITIONT, NORMAL, COLOR0-1, TEXCOORD0-7, PSIZE and FOG.
  - An FVF becomes a declaration internally.
  - Pretransformed vertices (XYZRHW / POSITIONT) are turned from screen space into NDC with the viewport, with w = 1/rhw.
  - Stream sources 0-15. `SetStreamSourceFreq` with any value other than 1 stops loudly.
- **Draws:**
  - `DrawPrimitive`, `DrawIndexedPrimitive` (with BaseVertexIndex and MinIndex), `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP`.
  - The UP data is copied into a per-frame upload ring. The UP calls unbind stream 0 and the indices afterwards, as documented.
  - Primitive types: point list, line list and strip, triangle list and strip, and triangle fan, which becomes a generated index list.
- **Rasterization:**
  - The D3D9 half-pixel convention is applied in the generated vertex function: the output position moves by (-1/w, +1/h) × pos.w, using the viewport size.
  - Winding: D3D `CULL_CW` culls clockwise triangles as seen on screen. It maps to Metal's front-facing winding and cull mode, and the tests pin which way.
- **Fixed-function vertex stage:**
  - The WORLD, VIEW and PROJECTION transforms.
  - Lighting (`D3DRS_LIGHTING`) with up to 8 directional, point and spot lights.
  - Material sources (`DIFFUSE`/`AMBIENT`/`SPECULAR`/`EMISSIVEMATERIALSOURCE`), `AMBIENT`, `SPECULARENABLE`, `LOCALVIEWER` and `NORMALIZENORMALS`.
  - `TEXCOORDINDEX` with PASSTHRU, CAMERASPACENORMAL, CAMERASPACEPOSITION, CAMERASPACEREFLECTIONVECTOR and SPHEREMAP.
  - `TEXTURETRANSFORMFLAGS` COUNT1-4 and PROJECTED.
  - Vertex fog (`FOGVERTEXMODE` NONE, EXP, EXP2 and LINEAR, with `RANGEFOGENABLE`).
  - User clip planes, through `clip_distance` (`SetClipPlane`, `CLIPPLANEENABLE`).
  - `VERTEXBLEND` other than DISABLE, and `INDEXEDVERTEXBLENDENABLE`, stop loudly.
- **Fixed-function pixel stage:**
  - 8 texture stages with every `D3DTOP` (MODULATE2X/4X, ADDSIGNED(2X), ADDSMOOTH, BLEND*ALPHA*, PREMODULATE, MODULATEALPHA_ADDCOLOR and its relatives, DOTPRODUCT3, MULTIPLYADD, LERP, BUMPENVMAP and BUMPENVMAPLUMINANCE with `BUMPENVMAT*`/`BUMPENVL*`).
  - Arguments DIFFUSE, CURRENT, TEXTURE, TFACTOR, SPECULAR, TEMP and CONSTANT, with the COMPLEMENT and ALPHAREPLICATE modifiers.
  - `RESULTARG`.
  - Specular add.
  - Table fog (`FOGTABLEMODE`, `FOGSTART`/`END`/`DENSITY`, `FOGCOLOR`).
  - Alpha test (`ALPHATESTENABLE`, `ALPHAFUNC`, `ALPHAREF`).
  - `SHADEMODE` FLAT, through a `[[flat]]` variant.
  - `SRGBWRITEENABLE`.
- **Pipeline and samplers:**
  - Blending (`ALPHABLENDENABLE`, SRC/DEST/BLENDOP, `SEPARATEALPHABLENDENABLE` and its alpha trio, `BLENDFACTOR`).
  - `COLORWRITEENABLE` 0-3.
  - Z (`ZENABLE`, `ZWRITEENABLE`, `ZFUNC`).
  - Stencil, including two-sided (`TWOSIDEDSTENCILMODE`, the CCW ops).
  - `DEPTHBIAS` and `SLOPESCALEDEPTHBIAS`.
  - `SCISSORTESTENABLE`.
  - `FILLMODE` WIREFRAME as Metal lines and POINT as points.
  - `POINTSPRITEENABLE` stops loudly.
  - Sampler states: ADDRESSU/V/W with WRAP, MIRROR, CLAMP, BORDER and MIRRORONCE. BORDER uses Metal's nearest border colour (transparent black, opaque black or opaque white) and logs once when the colour is another.
  - MAG/MIN/MIPFILTER including ANISOTROPIC and MAXANISOTROPY.
  - MIPMAPLODBIAS, through a sample `bias()` in the generated MSL.
  - MAXMIPLEVEL, through `lod_min_clamp`.
  - SRGBTEXTURE, through an sRGB view.
- **Caches:** redundant Set calls are skipped by comparing values, and keys are recomputed only for dirty groups. The MSL libraries and pipelines are cached by key in memory and in `MTLBinaryArchive` at `<support>/ShaderCache/ff.metalar` (my call). The archive is loaded at device creation and saved at device release and at process exit. Compiles are synchronous in this plan; plan 5 adds asynchronous compiles for guest shaders, where the volume is.
- **Programmable pipeline:** `SetVertexShader(NULL)` and `SetPixelShader(NULL)` are accepted. `Set/GetVertexShaderConstantF/I/B` and the pixel ones store values for plan 5. `CreateVertexShader` and `CreatePixelShader` exit 4.
- **Validation:** `ValidateDevice` is D3D_OK, 1 pass.

- [ ] **Step 1: Write the failing tests.** Each is a group and a test function. Expected pixels come from the D3D9 documentation's formulas, written into the test.
  - `tri_colour`: an XYZRHW triangle with diffuse colours, drawn into a 64x64 target. Pixel (10,50) inside is the interpolated colour within ±2 per channel; outside pixels keep the clear colour. The coverage edge follows the top-left rule: a pretransformed quad from (0,0) to (32,32) covers exactly 32x32 pixels.
  - `half_pixel`: a pretransformed quad at integer coordinates, textured from a 2x2 checker with point filtering, maps texel centres to pixel centres with no bleeding.
  - `transforms`: a world/view/projection cube drawn with Z on. Its silhouette pixel counts match a reference computed in the test from the same matrices.
  - `primitive_types`: each of the 6 types draws the expected pixels. A triangle fan of 4 triangles fills a hexagon's area.
  - `dynamic_discard`: 50 draws in one frame, each after `Lock(DISCARD)` writes a quad at a new x position. All 50 quads appear.
  - `stages`: for each D3DTOP, one quad with known texture, diffuse and TFACTOR values gives the formula's result within ±2.
  - `blend_depth_stencil`: every blend factor and op against a known destination; Z funcs; a stencil mask pass.
  - `lighting`: a directional light on a quad with a known normal gives `diffuse × N·L + ambient`; a point light with attenuation also matches.
  - `fog_alpha`: linear table fog at known depths; alpha test with GREATER at 0x80.
  - `samplers`: wrap, mirror and clamp at u=1.25; anisotropic filtering accepted; MAXMIPLEVEL 1 samples level 1.
  - `cache`: a second device in the same process with the same states compiles no new libraries (the cache counter is unchanged), and after an `ff.metalar` reload, pipeline creation hits the archive.
  - `depth_objects_draw`: completes Task 4's check with a Z-tested draw.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement**, one group at a time, rerunning that group each time.
- [ ] **Step 4: Run the whole `test_d3d9.py`.** Expected: all pass.
- [ ] **Step 5: Commit**, one commit per group of features as it goes green, for example `git commit -m "Draw D3D9 primitives through Metal"`, `git commit -m "Add fixed-function lighting and fog"`, `git commit -m "Add texture stage blending"`.

### Task 7: The rest of the device

**Files:**
- Modify: `runtime/bw2/d3d9/d3d9_device.c`, `d3d9_state.c`
- Create: `runtime/bw2/d3d9/d3d9_query.m`
- Test: `tests/test_d3d9.py`, `d3d9_selftest.c`

**Interfaces:**
- **State blocks:**
  - `CreateStateBlock(ALL/PIXELSTATE/VERTEXSTATE)`, with the state sets the D3D9 documentation lists for each type.
  - `BeginStateBlock` / `EndStateBlock`, which record only the states that were set.
  - The state block's `Capture`, `Apply` and `GetDevice`.
- **Lights, materials and transforms:** `SetLight` / `GetLight` / `LightEnable` / `GetLightEnable`, with any index; storage grows. `SetMaterial` / `GetMaterial`. `SetTransform` / `GetTransform` / `MultiplyTransform` for WORLD 0-255, VIEW, PROJECTION and TEXTURE0-7.
- **Queries:** `CreateQuery(EVENT)` and `CreateQuery(OCCLUSION)`. Their `Issue` and `GetData` are done with command buffer completion and Metal visibility results. `CreateQuery(NULL type)` reports support. Other types give D3DERR_NOTAVAILABLE.
- **Other device methods:**
  - `SetSoftwareVertexProcessing` / `GetSoftwareVertexProcessing`.
  - `SetNPatchMode(0)`; any other value stops loudly.
  - `GetClipStatus` / `SetClipStatus`.
  - `GetTextureStageState` for stages 0-7.
  - `SetPaletteEntries`, `GetPaletteEntries`, `SetCurrentTexturePalette` and `GetCurrentTexturePalette`, stored (no P8 textures exist in the table).
  - `CreateAdditionalSwapChain` gives a swap chain object whose `Present` presents its own back buffer into the main window.
  - `GetFrontBufferData` copies the last presented back buffer.
  - `ProcessVertices`, `DrawRectPatch`, `DrawTriPatch` and `DeletePatch` stop loudly.

- [ ] **Step 1: Write the failing tests.** Groups:
  - `stateblock`: capture, change, apply, and the states read back as captured. A recorded block applies only what was set.
  - `query_event`: Issue(END), then `GetData(FLUSH)` becomes S_OK within 1 s.
  - `query_occlusion`: a hidden quad behind a Z-written one counts 0 samples, and a visible one counts its pixel area.
  - `lights_grow`: `SetLight(100, ...)` reads back.
  - `swapchain2`: a second swap chain presents without error, and its back buffer reads back.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the whole `test_d3d9.py`.** Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Add state blocks, queries and the remaining device calls"`

### Task 8: D3DX math

**Files:**
- Create: `runtime/bw2/d3dx/d3dx_math.c` (registered in `register.c`; `engine_sources()` also globs `runtime/bw2/d3dx/`)
- Test: `tests/test_d3dx.py` (new), `runtime/bw2/d3dx/d3dx_selftest.c`

**Interfaces:**
- All 30 math imports in the import list of `d3dx-usage.md`: `D3DXMatrix*`, `D3DXVec2/3/4*`, `D3DXQuaternion*` and `D3DXFloat16To32Array`. Each is stdcall, returns its output pointer as D3DX does, and allows the output to alias an input.
- Formulas are the documented ones, in float, in D3DX's operation order where the documentation gives it:
  - `D3DXMatrixInverse` returns NULL for a singular matrix and writes the determinant when asked.
  - `D3DXVec3TransformCoord` divides by w.
  - `D3DXQuaternionSlerp` takes the short path when the dot product is negative, and lerps when the quaternions are nearly equal.
  - `D3DXQuaternionRotationYawPitchRoll` and `D3DXMatrixRotationQuaternion` use the documented conventions (left-handed, rows are basis vectors).
  - `D3DXVec3Project` and `D3DXVec3Unproject` take a viewport and three matrices, any of which may be NULL (identity).
  - `D3DXFloat16To32Array` handles denormals, infinities and NaN.

- [ ] **Step 1: Write the failing tests.** `test_d3dx.py` builds `d3dx_selftest` the way `test_d3d9.py` does, and runs group `math`. The checks are against expected values computed in plain Python in the test and passed to the selftest as arguments:
  - each function on 3 input sets, within 1e-5 relative;
  - aliasing (`out == in`) for multiply, inverse and transpose;
  - a singular inverse returns NULL;
  - a slerp of q with -q at t=0.5 is q.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "Add D3DX math"`

### Task 9: D3DX textures and buffers

**Files:**
- Create:
  - `runtime/bw2/d3dx/d3dx_image.c`: DDS, TGA, BMP and the codec choice;
  - `runtime/bw2/d3dx/d3dx_imageio.m`: PNG, JPEG and BMP decode and encode through ImageIO;
  - `runtime/bw2/d3dx/d3dx_tex.c`: create, load, filter, fill and save;
  - `runtime/bw2/d3dx/d3dx_buffer.c`: `ID3DXBuffer`.
- Test: `tests/test_d3dx.py`, `d3dx_selftest.c`

**Interfaces:**
- **`D3DXCreateBuffer`:** an `ID3DXBuffer` with `GetBufferPointer` and `GetBufferSize`, holding guest memory.
- **`D3DXCreateTexture` and `D3DXCreateCubeTexture`:**
  - `D3DX_DEFAULT` sizes become 256.
  - Sizes are kept, because the caps have no POW2.
  - `D3DFMT_UNKNOWN` becomes A8R8G8B8.
  - Levels 0 or `D3DX_DEFAULT` means a full chain.
- **`D3DXGetImageInfoFromFileInMemory`:** DDS (2D, cube, volume, the formats in the table, with and without mips), TGA (types 2, 3, 10 and 11, 8/16/24/32-bit, either origin), BMP, PNG and JPEG.
- **The `...FromFileInMemory` and `...FromFileInMemoryEx` loaders (2D, cube, volume) and `D3DXCreateTextureFromFileA`:**
  - They honour Width, Height, MipLevels, Format, Pool, Filter, MipFilter, ColorKey (an ARGB match becomes transparent black), and the out `D3DXIMAGE_INFO`.
  - `D3DX_FROM_FILE` and `D3DX_DEFAULT_NONPOW2` are honoured too.
  - DDS data in a matching format is copied without conversion.
  - The file version goes through win32hle's file API, so paths resolve in the guest's tree.
- **`D3DXLoadSurfaceFromMemory`, `D3DXLoadSurfaceFromSurface` and `D3DXLoadSurfaceFromFileInMemory`:**
  - They convert between every format in Task 5's table, except into DXT.
  - DXT is decoded on the CPU when the source is compressed and the destination is not.
  - Filters: POINT, LINEAR, TRIANGLE and BOX (TRIANGLE is done as BOX, my call). DITHER is ignored; MIRROR_* and SRGB_* are honoured.
  - Source and destination rects are supported.
- **`D3DXFilterTexture`:** BOX (the default) or POINT, from a source level down the chain, for 2D and cube.
- **`D3DXFillTexture` and `D3DXFillCubeTexture`:** for each texel, a `hle_call_guest` of the callback with (out vec4, texcoord of the texel centre, texel size, data), then conversion to the format. For cubes, texcoord is the documented direction vector.
- **Saving:** `D3DXSaveSurfaceToFileA`, `D3DXSaveTextureToFileA` and `D3DXSaveTextureToFileInMemory` write BMP, DDS, PNG, JPG and TGA. TGA is 32-bit uncompressed; PNG and JPG go through ImageIO. Files are written through win32hle, so they land under the support folder. The in-memory version returns an `ID3DXBuffer`.

- [ ] **Step 1: Write the failing tests.** Groups; the images are made by the test:
  - `dds`: a DXT1 DDS with 3 mips loads with levels intact (bytes compared after lock). An A8R8G8B8 cube DDS loads 6 faces.
  - `tga_bmp_png`: the same 5x3 image as TGA (raw and RLE, both origins), BMP and PNG loads to identical A8R8G8B8 texels.
  - `convert`: A8R8G8B8 to R5G6B5 to A8R8G8B8 gives the documented bit-replication values. DXT1 to A8R8G8B8 decodes a known block.
  - `filter`: box filtering a 4x4 checker gives grey at level 2.
  - `fill`: a guest callback (assembled with `asm32` into a tiny guest; or, in the selftest, a registered host shim at a guest VA) that writes (u, v, 0, 1) fills a 4x4 texture with texel-centre values.
  - `save`: saving to BMP and reloading gives identical texels, and the file lands under `<support>`.
  - `colorkey`: magenta becomes 0x00000000.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them**, then the whole `tests/test_d3d9.py tests/test_d3dx.py tests/test_engine.py`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Add D3DX texture loading and saving"`

### Task 10: Boot to plan 5's layer

**Files:**
- Create: `docs/research/graphics.md`
- Modify:
  - `tests/test_boot.py`: rename `test_boot_reaches_graphics` to `test_boot_reaches_shaders`;
  - the spec, sections 5.2 and 7.3, with what the boot showed;
  - `docs/research/boot.md`: "For the later plans" points to `graphics.md`.

**Interfaces:**
- Consumes everything above.
- Produces the measured record plan 5 starts from.

- [ ] **Step 1: Write the failing test.** `test_boot_reaches_shaders` runs `out/bw2` headless on the game tree and asserts:
  - exit code 4;
  - stderr names one of plan 5's methods or D3DX functions from Global Constraints;
  - no word from `LOUD`;
  - `"reached Direct3DCreate9"` is not in stderr.
- [ ] **Step 2: Run it.** `python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_boot.py -v`. Expected: FAIL at the first D3D9 or D3DX call this plan has not yet answered correctly.
- [ ] **Step 3: Bring-up loop.** Run `BW2_HEADLESS=1 BW2_TRACE_IMPORTS=1 out/bw2 --game gamefiles/game`. Each loud stop that is in this plan's layer is answered the way Tasks 3-9 define, with a selftest check added to the owning group first, red then green. Each stop outside every plan's layer, and each tripwire, stops the run and goes to the user, as in plan 3. Record each fix in the ledger.
- [ ] **Step 4: Measure and write `graphics.md`:**
  - the presentation parameters and creation flags the game asks for;
  - the caps fields and formats it queries;
  - the counts of each D3D9 method called;
  - the D3DX calls and what they load (file kinds and sizes, no content);
  - the first frame presented, if any;
  - the detail settings, if it saves `Settings.ini` or registry values before stopping (the open item from `boot.md`), and which caps, CPU and memory values feed them;
  - the time to the stop;
  - the stop itself.

  If the game would choose low detail because of a value this plan pinned (vendor, video memory, shader version), stop and report it to the user with the evidence, rather than guessing a new value.
- [ ] **Step 5: Run the full suite.** `.venv/bin/pytest -q > .superpowers/sdd/plan4-suite.txt 2>&1; tail -3 .superpowers/sdd/plan4-suite.txt`. Expected: all pass.
- [ ] **Step 6: Audit and commit.** `third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through Direct3D to the shader layer"`
