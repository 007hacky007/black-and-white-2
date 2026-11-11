# Plan 5: Shaders and effects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game gets past `D3DXCreateEffect` and renders through its own shaders:
- A minimal DirectInput mouse answers the game's input setup.
- Vertex and pixel shader bytecode (vs 1.1-2.0, ps 1.1-2.0) is translated to Metal Shading Language and drawn with D3D9 semantics.
- The D3DX shader assembler and the fx_2_0 effect framework (parameters, pools, expressions and preshaders, techniques, passes, states) run natively.

The boot then stops at the next layer no plan has answered yet.

**Architecture:**
- **Bytecode in.** Shader bytecode is decoded once into a small instruction list (`sm.h`). Each stage gets one generated MSL function. Vertex functions write the same `VOut` interface plan 4's fixed-function vertex function writes, and fragment functions read it. So a shader stage pairs with a fixed-function stage of the other kind in one pipeline, as D3D9 allows.
- **Compiling and caching.** Each shader's MSL is compiled on a background queue when the shader is created. Pipeline states are keyed and stored in plan 4's binary archive.
- **The assembler** turns text into the same bytecode the game's shaders use. The tests write their shaders in assembly, so every translator test runs real D3D9 bytecode.
- **Effects** are guest COM objects on plan 4's `d3d_com` machinery. An effect parses the fx_2_0 binary into host structures. Its CPU expression bytecode (FXLC: state expressions and shader preshaders) runs in a small float32 interpreter. A pass is applied through the device's own host-side setters, so state blocks, Get calls and draws see exactly what the game would see.
- **Phasing (user, 2026-11-11):** shaders and effects come before input, because the game reads no input before this stage. Full input is plan 6 (`2026-11-11-bw2-plan6-input.md`, deferred). HLSL compilation, the fragment linker, sprite, font and mesh are plan 7's, unless this plan's boot reaches them first. Then they stop as a later plan's layer.

**Tech Stack:** C and Objective-C (clang, arm64, ARC, macOS 13+), Metal, pcrecomp win32hle (vendored, MIT), Python 3.14 and pytest.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 5.2 D3D9 and D3DX, 6.4, 7.3). Facts: `docs/research/d3dx-usage.md`, `docs/research/graphics.md`.

## Measured before writing (2026-11-11)

- **The boot with a do-nothing mouse** (throwaway runs, not kept):
  - After input setup, the game creates a full-screen device (1024x768, `D3DFMT_A8R8G8B8`, `D3DFMT_D24S8`, 60 Hz, immediate interval).
  - It then records two state blocks and creates two textures.
  - It reads `Data/effects/DownScaleFilter.sdv` and calls `D3DXCreateEffect(device, data, 1804, NULL, NULL, 0, NULL, &effect, &errors)`. The data is the **compiled** fx_2_0 binary (first dword `0xFEFF0901`). There is no pool and no defines.
  - It pumps no messages and reads no input before that call.
- **DirectInput setup** (`0x9AF220`; the full record is in plan 6's "Measured"):
  - `DirectInput8Create(.., 0x800, IID_IDirectInput8A, ..)`;
  - `CreateDevice(GUID_SysMouse)` (slot 3);
  - `SetDataFormat` (slot 11) with the game's copy of `c_dfDIMouse` (24, 16, 2, 16, 7 objects);
  - `SetCooperativeLevel(hwnd, 6)` (slot 13);
  - `SetProperty(DIPROP_BUFFERSIZE, 512)` (slot 6);
  - `Acquire` (slot 7).

  Later it calls `Unacquire` (8), `SetCooperativeLevel(.., 5)`, `GetDeviceData(20, .., 512, 0)` (10) and `GetDeviceState(16, ..)` (9).
- **The 59 `Data/effects/*.sdv` files** (game tree after patches):
  - Each is a Lionhead container holding the effect's HLSL source text (about 98% printable, 2-12 KB) and then one fx_2_0 binary. The game passes the binary.
  - Inside the binaries:
    - 2,645 parameters: float 207 scalars, 749 vectors and 197 matrices; 11 bool, 6 int; 812 textures; 804 samplers; 74 structs; 4 pixelshader and 3 vertexshader parameters; 58 arrays.
    - 322 parameters are flagged shared (an effect pool).
    - 165 techniques, 172 passes, 88 technique annotations.
  - Pass state operations seen: 0, 1, 3, 4, 6, 7, 8, 9, 10, 11, 13, 14, 22, 26, 30, 48, 59, 73, 75, 103, 105, 107, 109, 116, 119, 123, 124, 125, 146, 147, 164. 146 and 147 each occur 168 times: `VertexShader` and `PixelShader` once per pass.
  - Shaders (size-checked blobs): vs_1_1 22, vs_2_0 20, ps_1_1 17, ps_1_4 4, ps_2_0 19. No 3.0 shader.
  - Instructions used:
    - vs: add, abs, dcl, def, dp3, dp4, expp, frc, lrp, mad, max, min, mov, mova, mul, nrm, rcp, rsq, sincos, slt.
    - ps 1.x: add, cmp, cnd, def, dp3, lrp, mad, mov, mul, phase, tex, texcoord.
    - ps 2.0: abs, add, cmp, dcl, def, dp2add, dp3, dp4, lrp, mad, max, mov, mul, nrm, pow, rsq, texld, texkill.
    - No flow control.
  - Modifiers used:
    - destination: saturate, partial precision, both; shifts _x2 and _x4;
    - source: negate (296), bx2 (8), comp (2), x2 (2), dw (1);
    - relative addressing: 48 (constant arrays).
  - Every sampler `dcl` is 2D.
  - Chunks: 435 `CTAB`. 243 `PRES`, 117 `FXLC`, 117 `CLIT` and 104 `PRSI`, so preshaders and state expressions are present.
  - Blobs with version `0x4658xxxx` (FX expression): 8 of 2.0 and 82 of 2.1. Four `0x5458` (TX) blobs.
- **`Data/EncryptedShaders`:** 17 `.vsh` (vs.1.1 11, vs.2.0 5, 1 without a version line) and 12 `.psh` (ps.2.0 9, ps.1.1 1, ps.1.4 1, 1 without) as text assembly, and 7 `.fx` HLSL sources. Each byte is shifted by +3; the game undoes the shift before calling D3DX.
- **Plan 4 today:**
  - `CreateVertexShader`/`CreatePixelShader` exit 4.
  - Shader constants F/I/B are stored.
  - Caps advertise vs_3_0/ps_3_0.
  - The fixed-function `VOut` is `pos`, `c0`, `c1`, `tc0..tc(n-1)`, `fog`, `fogz`, and optionally `psize` and `clip[]`.
  - Uniforms are at `buffer(0)` and streams from `buffer(8)`.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings, bytes, shaders, effects or text copied from them, or generated C. Facts only (addresses, sizes, counts, hashes). Every shader and effect in a committed test is written by the test, and game files are read only by tests that need `BW2_GAMEFILES=1`. Run `third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data.
- **Standing rule (user):** if the VM or the code it runs asks for a disc or driver check, stop and ask the user. The tripwire stays on, and its allow list stays empty. Winsock keeps answering "no network".
- Fail loudly, never return 0 silently:
  - An unimplemented method stops with `d3d9: <Interface>::<Method> not implemented (from guest 0x%08X)`.
  - An unsupported shader feature stops naming the shader version, the instruction (or register type, or modifier) and its token offset. That covers vs/ps 3.0, vs_2_x/ps_2_x extensions, predication and dynamic flow control.
  - An FXLC opcode the interpreter lacks stops naming it.
  - An effect state operation with no table entry stops naming its number.
- **Plan 7 owns these.** Each exits 4 with `[bw2] reached <name>: owned by a later plan`:
  - `D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXCreateFragmentLinker`, `D3DXCreateSprite`, `D3DXCreateFontA`, `D3DXCreateSphere`;
  - `D3DXCreateEffect` given HLSL text (data not starting with `0xFEFF0901`), reported as `D3DXCreateEffect (HLSL source)`.

  DirectSound/OpenAL and Bink stay as in plan 3.
- **Shader model (my call): the device now advertises shader model 2.0**, which is everything the game ships. That covers `VertexShaderVersion` `0xFFFE0200` and `PixelShaderVersion` `0xFFFF0200`. Also:
  - `VS20Caps` {Caps 0, DynamicFlowControlDepth 0, NumTemps 12, StaticFlowControlDepth 1};
  - `PS20Caps` {Caps 0, DynamicFlowControlDepth 0, NumTemps 12, StaticFlowControlDepth 0, NumInstructionSlots 96};
  - `MaxVShaderInstructionsExecuted` 65535 and `MaxPShaderInstructionsExecuted` 96;
  - 3.0 instruction slots 0;
  - `MaxVertexShaderConst` 256, `MaxPixelShaderConst` (ps_2_0) 32.

  `D3DXGetVertexShaderProfile` gives `"vs_2_0"` and `D3DXGetPixelShaderProfile` gives `"ps_2_0"`. The game's only shader-version test (`PixelShaderVersion >= ps_2_0`, `0x933C0D`) still passes.
- **D3D9 shader semantics pinned here (from the D3D9 shader reference):**
  - `rcp(0)` and `rsq(0)` give +infinity; `rsq` and `log` use `|x|`; `log(0)` gives -infinity.
  - `pow(x, y)` is `pow(|x|, y)`.
  - `nrm` of a zero vector gives 0 (my call, to avoid NaN).
  - vs_1_1 `mov a0.x` is `floor`, and vs_2_0 `mova` rounds to nearest, ties away from zero.
  - A relative constant read outside 0..255 gives (0,0,0,0).
  - vs < 3.0 colour outputs `oD0`/`oD1` are clamped to [0,1].
  - ps 1.x:
    - the result is `r0`;
    - `texcoord` clamps to [0,1];
    - `cnd` in 1.1-1.3 tests `r0.a > 0.5` (the only source 0 allowed), in 1.4 each component `> 0.5`;
    - `cmp` is `src0 >= 0`.
  - The source modifiers bias (x-0.5), bx2 (2x-1), comp (1-x), x2, negate, abs, dz and dw behave as documented, as do the destination shifts (_x2 _x4 _x8 _d2 _d4 _d8) and saturate. Partial precision and centroid are accepted and ignored.
  - The half-pixel move of plan 4 (+1/w, -1/h)·w applies to `oPos`.
  - With a vertex shader, user clip planes are in clip space.
  - Fog with ps < 3.0 is D3D9's fixed fog applied after the shader: vertex fog from `oFog`, or table fog. The alpha test is also applied after the shader. Both reuse plan 4's code.
- **Buffers in translated MSL:**
  - `buffer(0)`: plan 4's uniforms (`ff_vs_u` / `ff_ps_u`).
  - `buffer(1)`: float constants (vs `float4[256]`, ps `float4[32]`).
  - `buffer(2)`: `int4 i[16]` then `uint b[16]`.
  - Streams stay from `buffer(8)`. Textures and samplers stay at the stage's index.
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES`. No NFSU2Recomp code. The translator, assembler and effect framework are written from the D3D9 and D3DX documentation, and from what the game's files show.
- Exit codes are unchanged: 0 normal, 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- Headless (`BW2_HEADLESS=1`) for every test, as in plan 4. Pixel tests draw into offscreen targets and read them back.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Identity is 007hacky007. Never push. Work on branch `plan5`.

## Review Focus

- **A shader stage paired with the fixed-function stage of the other kind** (vs + fixed pixel pipeline, or fixed vertex pipeline + ps): the interface matches, and colours, texture coordinates and fog arrive as D3D9 defines them. (Task 4, group `mixed_stages`.)
- **Relative constant addressing at the array's ends** (skinning palettes: `c[a0.x + n]` with a0 at -1, 0 and the last index): the in-range reads are exact, and the out-of-range ones give zero, never memory outside the array. (Task 3, group `vs_reladdr`.)
- **The same effect parameter shared through a pool by two effects, with one set between passes:** both see the new value at their next `BeginPass` or `CommitChanges`, and an unshared parameter with the same name stays separate. (Task 5, group `fx_pool`.)
- **`CommitChanges` after setting a parameter used only by a preshader:** the preshader re-runs and the derived constant reaches the shader. Parameters that did not change are not re-uploaded. (Task 6, group `fx_preshader_commit`.)
- **`Begin` without `D3DXFX_DONOTSAVESTATE`, then `End`:** every state the passes changed, and the shaders, are back as before. With the flag, they are left as the last pass set them. (Task 6, group `fx_save_restore`.)

---

### Task 1: A minimal DirectInput mouse

**Files:**
- Create: `runtime/bw2/input/dinput8.c`, `runtime/bw2/input/input_selftest.c`, `tests/test_input.py`.
- Modify:
  - `bw2/build.py` (`BW2_SUBDIRS` gains `"input"`);
  - `runtime/bw2/d3d9/d3d_com.h` (kinds `D3DK_DINPUT8`, `D3DK_DIDEVICE`);
  - `runtime/bw2/d3d9/d3d9_methods.c` (interfaces `IDirectInput8A`, `IDirectInputDevice8A` in their header declaration order: 11 and 32 methods);
  - `runtime/bw2/register.c` (`DirectInput8Create`);
  - `runtime/bw2/later.c` (drop `dinput8.dll` and `DirectInput8Create`).

**Interfaces:**
- Consumes: plan 4's `d3d_new`, `d3d_host`, `d3d_impls`, `d3d_vtable`.
- Produces `DirectInput8Create` and a mouse that answers the measured calls and **reports no input**. Plan 6 replaces the reads and keeps the rest.
- **Values:**
  - `IID_IDirectInput8A` {BF798030-483A-4DA2-AA99-5D64ED369700}.
  - `GUID_SysMouse` {6F1D2B60-D5A0-11CF-BFC7-444553540000}.
  - HRESULTs: `DI_OK` 0, `DI_NOEFFECT` 1, `DIERR_INVALIDPARAM` 0x80070057, `DIERR_NOINTERFACE` 0x80004002, `DIERR_OLDDIRECTINPUTVERSION` 0x8007047E, `DIERR_BETADIRECTINPUTVERSION` 0x80070481, `CLASS_E_NOAGGREGATION` 0x80040110, `DIERR_ACQUIRED` 0x800700AA, `DIERR_NOTACQUIRED` 0x8007000C.
- **`DirectInput8Create`:** version 0x800, riid `IID_IDirectInput8A`, outer NULL. Otherwise the documented errors above.
- **IDirectInput8A:** `CreateDevice(GUID_SysMouse)` gives the device. Any other GUID stops loudly, naming it. Every other method stops loudly.
- **IDirectInputDevice8A:**
  - `SetDataFormat`: accepts a format with dwSize 24, dwObjSize 16 and dwDataSize 16 or 20; otherwise `DIERR_INVALIDPARAM`.
  - `SetCooperativeLevel`: flags 5 or 6 (and 9/10 background) accepted; others stop loudly.
  - `SetProperty`: `DIPROP_BUFFERSIZE` ((GUID*)1, a 20-byte DIPROPDWORD with DIPH_DEVICE); others stop loudly.
  - `SetDataFormat`, `SetCooperativeLevel` and `SetProperty` while acquired give `DIERR_ACQUIRED`.
  - `Acquire` gives `DI_OK`, or `DI_NOEFFECT` if already acquired. `Unacquire` gives `DI_OK`, or `DI_NOEFFECT` if not acquired.
  - Reads while not acquired give `DIERR_NOTACQUIRED`.
  - `GetDeviceData(cb 16 or 20, ..., &n, 0 or 1)` sets n to 0 and gives `DI_OK`.
  - `GetDeviceState(cb == dwDataSize)` zero-fills and gives `DI_OK`.
  - The first read prints `[bw2] DirectInput mouse: no input until plan 6` once.
  - Every other method stops loudly.

- [ ] **Step 1: Write the failing tests.** `tests/test_input.py` compiles `input_selftest` like `tests/test_d3d9.py`'s fixture. Groups:
  - `di_create`: the version, riid and outer rules.
  - `di_game_setup`: the measured sequence with the game's format built by the test. Each call gives `DI_OK`. Then `Unacquire`, coop 5 and `Acquire` give `DI_OK`, a `GetDeviceData(20, buf, &n=512, 0)` gives n 0, `GetDeviceState(16)` gives zeros, and `Release` gives 0.
  - `di_rules`: `DIERR_ACQUIRED` for setup calls while acquired; `DIERR_NOTACQUIRED` for reads before `Acquire`.
  - `di_loud`: `GetCapabilities` stops with `IDirectInputDevice8A::GetCapabilities not implemented`; `CreateDevice` of another GUID stops naming it.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_input.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/python -m bw2.pipeline --build-only && .venv/bin/pytest tests/test_input.py tests/test_d3d9.py tests/test_engine.py -q`. Expected: all pass. Then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: `test_boot_reaches_input` now FAILS, because the boot goes on to `D3DXCreateEffect`. Rename it to `test_boot_reaches_effects`, asserting exit 4 with `reached D3DXCreateEffect`, and it passes. Task 7 replaces it.
- [ ] **Step 5: Commit.** `git commit -m "Answer the game's DirectInput mouse setup"`

### Task 2: The shader assembler

**Files:**
- Create:
  - `runtime/bw2/d3d9/sm.h`: shader tokens, the decoded form, and the API for Tasks 2-6;
  - `runtime/bw2/d3d9/sm_decode.c`: bytecode to `struct sm_shader`, and validation;
  - `runtime/bw2/d3dx/d3dx_asm.c`: text to bytecode, with `D3DXAssembleShader`, `D3DXGetShaderVersion`, `D3DXGetVertexShaderProfile`, `D3DXGetPixelShaderProfile`;
  - `runtime/bw2/d3d9/sm_selftest.c`, `tests/test_shaders.py` (groups as `sm_selftest <group>`).
- Modify: `runtime/bw2/later.c` (drop the four names above), `runtime/bw2/d3dx/d3dx.h`.

**Interfaces:**
- Produces (`sm.h`):
  - `struct sm_reg { uint8_t type; uint16_t index; uint8_t mask_or_swizzle; uint8_t mod; int8_t shift; uint8_t rel; uint8_t rel_type; uint16_t rel_index; uint8_t rel_comp; };`
  - `struct sm_ins { uint16_t op; uint8_t ndst, nsrc; struct sm_reg dst, src[4]; uint8_t coissue, sat, pp; uint32_t usage, usage_index, sampler_type; float def[4]; int32_t defi[4]; uint32_t offset; };` where offset is the token index, for errors.
  - `struct sm_shader { uint32_t version; uint32_t nins; struct sm_ins *ins; uint64_t hash; /* FNV-1a 64 of the tokens */ ... };`
  - `int sm_decode(const uint32_t *tokens, uint32_t max_tokens, struct sm_shader *out, char *err, size_t errlen);`: 0, or -1 with a message naming the problem. It accepts vs_1_1, vs_2_0, ps_1_1-ps_1_4 and ps_2_0. 3.0, 2_x and anything outside those models fail.
  - `void sm_free(struct sm_shader *s);`
  - `int sm_assemble(const char *text, size_t len, const struct sm_define *defines, uint32_t **tokens, uint32_t *ntokens, char **errors);`
- **The assembler:**
  - Syntax: D3D9's `vs_1_1`/`vs.1.1` style headers through `ps_2_0`, every instruction of those models (with `_sat`, `_pp`, `_x2`/`_x4`/`_x8`/`_d2`/`_d4`/`_d8` and the source modifiers), `dcl_*` (usages and sampler types), `def`/`defi`/`defb`, `+` co-issue, `phase`, and comments `;` and `//`.
  - The preprocessor handles `#define NAME value`, `#undef`, `#ifdef`/`#ifndef`/`#else`/`#endif`, and the defines passed in. `#include` stops loudly for now, naming the file.
  - Token encoding follows the D3D9 token format. Instruction length bits 24-27 are written for 2.0 and are 0 for 1.x. The output carries no comment chunks.
  - On error: `E_FAIL` (0x80004005), and an `ID3DXBuffer` of `"(line): error: <what>"` lines through `ppErrorMsgs`.
- `D3DXAssembleShader(src, len, defines, include, flags, &shader, &errors)` returns an `ID3DXBuffer` (plan 4's) holding the tokens.
- `D3DXGetShaderVersion(tokens)` gives `tokens[0]`, or 0 for NULL.

- [ ] **Step 1: Write the failing tests** (`tests/test_shaders.py`, each running a group):
  - `asm_tokens`: `"vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\n"` assembles to exactly `FFFE0101 0000001F 80000000 900F0000 00000014 C00F0000 90E40000 A0E40000 0000FFFF`. Also:
    - `"ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n"` gives the documented tokens, written out in the test;
    - `"ps_1_1\ntex t0\nmul_x2 r0, t0, v0_bx2\n"` gives the shift and modifier bits.
  - `asm_errors`: an unknown opcode gives `E_FAIL` and an error buffer naming line 2. A `#define` substitutes. A defines array substitutes.
  - `decode_roundtrip`: every assembled test shader decodes, and `sm_decode` of a vs_3_0 header fails naming `vs_3_0`.
  - `asm_game_files` (`BW2_GAMEFILES=1` only): each of the 29 `.vsh`/`.psh` files, shifted back by 3, assembles with no error and decodes. This also counts and prints the versions, so they can be checked against the "Measured" counts above.
  - `profiles`: the two profile strings, and `D3DXGetShaderVersion`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_shaders.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement** `sm_decode.c` and `d3dx_asm.c`.
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_shaders.py tests/test_d3dx.py -q` and `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_shaders.py -k game -v`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Add the D3DX shader assembler"`

### Task 3: Vertex shaders on Metal

**Files:**
- Create: `runtime/bw2/d3d9/sm_msl.c` (MSL for both stages: this task the vertex side), `runtime/bw2/d3d9/d3d9_shader.c` (the shader objects).
- Modify:
  - `runtime/bw2/d3d9/d3d9_draw.c`: create, set and get vertex shaders, and the draw path;
  - `runtime/bw2/d3d9/mtl_pipeline.m`: functions from separate libraries, and the pipeline key;
  - `runtime/bw2/d3d9/d3d9_adapter.c`: the shader model 2.0 caps;
  - `runtime/bw2/d3d9/ff_msl.c`: the `VOut` interface as one shared emitter;
  - `sm_selftest.c`, `tests/test_shaders.py`, `tests/test_d3d9.py` (the caps group's expected values).

**Interfaces:**
- Consumes: Task 2's `sm_decode`, `sm_assemble`; plan 4's device, state, constants and `mtl_draw`.
- Produces:
  - `char *sm_msl_vertex(const struct sm_shader *s, const struct sm_vs_variant *v);`: one `vertex VOut vsh(...)` function. The variant holds the vertex declaration's usage-to-attribute map, the clip plane count, and whether points are drawn.
  - `char *sm_msl_fragment(const struct sm_shader *s, const struct sm_ps_variant *v);`: Task 4.
  - The device kinds `D3DK_VSHADER`/`D3DK_PSHADER` hold the decoded shader, its tokens (for `GetFunction`) and an async library handle `mtl_obj mtl_library_async(const char *msl, const char *name)`. The first draw that needs the library waits for it.
  - `mtl_draw` takes `d->vs` / `d->ps` (NULL means fixed function for that stage). The pipeline key is (vs hash or ff vertex key) x (ps hash or ff pixel key) x the variant and fixed state, as plan 4's is.
- **Vertex inputs:** each `dcl_<usage><n> v#` reads the vertex declaration element with that usage and index. A declaration element the shader does not read is ignored. A `dcl` with no element reads (0,0,0,1), as D3D9 does.
- **Outputs:**
  - `oPos` gives `pos`, after the half-pixel move;
  - `oD0`/`oD1` give `c0`/`c1`, clamped;
  - `oT0-7` give `tc0-7`;
  - `oFog` gives `fog`;
  - `oPts` gives `psize`.

  An output the shader does not write reads as follows (my call, recorded as a ruling to check against the game): `oD0`/`oD1` (1,1,1,1), `oT#` (0,0,0,0), `oFog` 1.0 (unfogged), `oPts` the `D3DRS_POINTSIZE` state.
- **vs_2_0 static flow control:** `if b#`/`else`/`endif`, `loop aL, i#`/`endloop`, `rep i#`/`endrep`, and `call`/`callnz b#`/`label`/`ret`. Each label becomes an MSL function taking the register file by reference.

- [ ] **Step 1: Write the failing tests** (pixel tests through `vcall` on a device, headless, offscreen 64x64 target, tolerance 1/255; groups in `tests/test_shaders.py`):
  - `vs_basic`: `m4x4 oPos, v0, c0` with an identity matrix in c0-c3 and `mov oD0, c4` draws c4's colour over the expected pixels, with the fixed-function pixel pipeline (stage 0 `SELECTARG1 DIFFUSE`).
  - `vs_ops`: each vs instruction the game uses writes its D3D9 result into `oD0` from constants chosen so each result is in [0,1]. That covers add abs dp3 dp4 expp frc lrp mad max min mov mul nrm rcp rsq sincos slt, plus `lit`, `dst`, `exp`, `log`, `sge`, `m3x3`, `m3x4`, `m4x3`, `m3x2`, `crs`, `sgn` and `pow`. Each case is one draw read back at one pixel, with its expected value in the test. Also `rcp(0)` (checked as > 1e30) and `nrm(0)` (0).
  - `vs_reladdr`: a 20-entry constant array read with `mova a0.x` (vs_2_0) and `mov a0.x` (vs_1_1) at the ends; out of range gives 0.
  - `vs_flow`: `if b0` with b0 true and false; `loop aL, i0` summing `c[aL]`; `rep`; `call`/`ret`.
  - `vs_inputs`: a declaration with POSITION, NORMAL, TEXCOORD1 and COLOR0 feeds `dcl_texcoord1 v3` and the others to the right registers; a `dcl_normal` with no element reads (0,0,0,1).
  - `vs_halfpixel`: a 1:1 textured quad drawn through a vertex shader samples texel centres exactly, as plan 4's fixed-function test does.
  - `vs_clip`: one user clip plane in clip space removes half a quad.
  - `caps_sm2`: the caps values in Global Constraints (`tests/test_d3d9.py::test_caps` updated).
  - `shader_objects`: `GetFunction` gives the tokens back, `GetDevice`, reference counts, `SetVertexShader(NULL)` returns to fixed function, and `GetVertexShader` AddRefs.
  - `vs_cache`: a second device draws the same shader with no new MSL compile (`mtl_get_stats`). After `mtl_pipeline_cache_drop` and a reload, the pipeline comes from the archive.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_shaders.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_shaders.py tests/test_d3d9.py tests/test_d3dx.py -q`. Expected: all pass, plan 4's groups included.
- [ ] **Step 5: Commit.** `git commit -m "Translate vertex shaders to Metal"`

### Task 4: Pixel shaders on Metal

**Files:**
- Modify: `runtime/bw2/d3d9/sm_msl.c` (the fragment side), `d3d9_shader.c`, `d3d9_draw.c`, `mtl_pipeline.m`, `sm_selftest.c`, `tests/test_shaders.py`.

**Interfaces:**
- Consumes: Task 3's objects, pipeline key and `VOut`; plan 4's fog and alpha-test MSL.
- Produces: `sm_msl_fragment` with `struct sm_ps_variant`. The variant holds per stage the bound texture type (for ps 1.x, which has no sampler `dcl`), the fog mode, the alpha function, flat shading and sRGB write. It forms part of the pipeline key.
- **ps 1.1-1.3:** `tex`, `texcoord`, `texkill`, `texbem`/`texbeml`, `texreg2ar`/`gb`/`rgb`, `texm3x2pad`/`tex`, `texm3x3pad`/`tex`/`spec`/`vspec`, `texdp3`/`texdp3tex`/`texm3x3`/`texm3x2depth`, plus the arithmetic. `t#` samples stage # with texture coordinate #. The bump-mapping instructions read plan 4's per-stage bump matrix.
- **ps 1.4:** `texld r#, t#|r#` (with _dz/_dw), `texcrd`, `phase`, `bem`, and `cnd` per component.
- **ps 2.0:** `dcl` of `v#`/`t#` and the 2D/cube/volume samplers, `texld`/`texldp`/`texldb`, `texkill`, and the arithmetic. The result is `oC0`, with `oDepth` written to the depth.
- Fog and the alpha test follow the shader, from the variant.

- [ ] **Step 1: Write the failing tests** (pixel tests as in Task 3):
  - `ps_ops`: each ps instruction the game uses in 1.x and 2.0 gives its documented result at a pixel. That covers add cmp cnd dp3 lrp mad mov mul (1.1), plus abs dp2add dp4 max nrm pow rsq (2.0). Also `cnd` in 1.1 (`r0.a`) against 1.4 (per component).
  - `ps_modifiers`: bias, bx2, comp, x2, negate, abs and dw on a texture read; the shifts _x2/_x4/_d2 and saturate; and co-issue (`+`) of an rgb and an alpha instruction.
  - `ps_tex`: ps_1_1 `tex t0` with a 2D texture and with a cube texture (a variant each); ps_1_4 `texld r0, t0` after `phase`; ps_2_0 `texld`, `texldp` (projected) and `texkill` (killed pixels keep the clear colour).
  - `ps_bump`: `texbem` with a bump matrix moves the sample by the documented offset.
  - `ps_fog_alpha`: a ps_2_0 shader with vertex fog from `oFog` and linear fog blends toward the fog colour. Alpha test GREATER 0x80 drops pixels from a shader alpha of 0.25.
  - `mixed_stages`: fixed vertex pipeline (diffuse and one texcoord) + ps_2_0 reading `v0` and `t0`; vs_1_1 + the fixed pixel pipeline (`MODULATE TEXTURE DIFFUSE`). Each gives the colour D3D9 defines.
  - `ps_srgb`: `D3DRS_SRGBWRITEENABLE` with a shader writes sRGB-encoded values.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_shaders.py tests/test_d3d9.py -q`. Then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_shaders.py -k game -v` with one more `BW2_GAMEFILES` group, `game_shaders_compile`: every shader blob in the 59 effects (found by size-checked version tokens, as in "Measured") decodes, translates and compiles to a Metal library with no error. Expected: all pass, and the counts match "Measured" (vs_1_1 22, vs_2_0 20, ps_1_1 17, ps_1_4 4, ps_2_0 19).
- [ ] **Step 5: Commit.** `git commit -m "Translate pixel shaders to Metal"`

### Task 5: Effects: the fx_2_0 binary, parameters and pools

**Files:**
- Create:
  - `runtime/bw2/d3dx/fx_parse.c`: the binary into host structures;
  - `runtime/bw2/d3dx/fx_effect.c`: `ID3DXEffect` and `ID3DXBaseEffect`'s parameter methods, `D3DXCreateEffect` and `D3DXCreateEffectPool`;
  - `runtime/bw2/d3dx/fx.h`;
  - `tests/fxwrite.py`: a test-side fx_2_0 writer for synthetic effects;
  - `runtime/bw2/d3dx/fx_selftest.c`;
  - `tests/test_effects.py`.
- Modify: `d3d_com.h` (kinds `D3DK_EFFECT`, `D3DK_EFFECTPOOL`), `d3d9_methods.c` (interfaces `ID3DXEffect`, `ID3DXEffectPool`), `later.c` (drop the two names).

**Interfaces:**
- Consumes: Task 2's decoder; plan 4's `ID3DXBuffer`.
- Produces (`fx.h`):
  - `struct fx_param` holds name, semantic, type, class, rows, columns, elements, members, annotations, flags (shared), a value (bytes, or an object handle), a dirty serial, and the guest handle.
  - `struct fx_effect` holds parameters, techniques, passes, states, objects (shaders, strings), and the pool.
  - `int fx_parse(const uint8_t *data, uint32_t len, struct fx_effect *out, char *err, size_t errlen);`
  - `uint32_t fx_handle(const struct fx_param *p);`
  - `struct fx_param *fx_param_of(struct fx_effect *e, uint32_t handle);`
- **The vtable** is d3dx9_25's `ID3DXEffect` order. First ID3DXBaseEffect's methods:
  - `QueryInterface`, `AddRef`, `Release`;
  - `GetDesc`, `GetParameterDesc`, `GetTechniqueDesc`, `GetPassDesc`, `GetFunctionDesc`;
  - `GetParameter`, `GetParameterByName`, `GetParameterBySemantic`, `GetParameterElement`, `GetTechnique`, `GetTechniqueByName`, `GetPass`, `GetPassByName`, `GetFunction`, `GetFunctionByName`, `GetAnnotation`, `GetAnnotationByName`;
  - `SetValue`, `GetValue`, `SetBool`, `GetBool`, `SetBoolArray`, `GetBoolArray`, `SetInt`, `GetInt`, `SetIntArray`, `GetIntArray`, `SetFloat`, `GetFloat`, `SetFloatArray`, `GetFloatArray`, `SetVector`, `GetVector`, `SetVectorArray`, `GetVectorArray`;
  - `SetMatrix`, `GetMatrix`, `SetMatrixArray`, `GetMatrixArray`, `SetMatrixPointerArray`, `GetMatrixPointerArray`, `SetMatrixTranspose`, `GetMatrixTranspose`, `SetMatrixTransposeArray`, `GetMatrixTransposeArray`, `SetMatrixTransposePointerArray`, `GetMatrixTransposePointerArray`;
  - `SetString`, `GetString`, `SetTexture`, `GetTexture`, `GetPixelShader`, `GetVertexShader`, `SetArrayRange`.

  Then `GetPool`, `SetTechnique`, `GetCurrentTechnique`, `ValidateTechnique`, `FindNextValidTechnique`, `IsParameterUsed`, `Begin`, `BeginPass`, `CommitChanges`, `EndPass`, `End`, `GetDevice`, `OnLostDevice`, `OnResetDevice`, `SetStateManager`, `GetStateManager`, `BeginParameterBlock`, `EndParameterBlock`, `ApplyParameterBlock`, `CloneEffect`.
  - **Step 1 confirms this order against the game:** disassemble the uses of the effect pointers stored after the three `D3DXCreateEffect` sites (`0x96F4BB`, `0x97CDB9`, `0x97E1EA`). Each slot the game calls must name a method whose arguments match the call. A mismatch is fixed in `d3d9_methods.c`, and the evidence goes in the ledger.
- **Handles (D3DX semantics):** a handle is the guest address of a small record the effect owns, one per parameter, annotation, technique, pass and element. Any other non-NULL value is read as a guest string and looked up by name. Structure members are found as `"struct.member"`, and array elements through `GetParameterElement`.
- **The parameter methods** (`GetParameter*`, `GetAnnotation*`, `Set`/`Get` for Value, Bool, Int, Float, Vector, Matrix (and Transpose and pointer arrays), String and Texture, plus the arrays and `GetParameterDesc`/`GetTechniqueDesc`/`GetPassDesc`/`GetDesc`) follow D3DX's conversions:
  - a bool reads 0/1, and a float set on an int truncates;
  - matrices are stored as declared (row or column major) and converted at Set/Get;
  - a texture parameter AddRefs its texture;
  - a type mismatch gives `D3DERR_INVALIDCALL`.
- **The pool:** `D3DXCreateEffectPool` gives a pool object. Parameters flagged shared, in effects created with the same pool, share one value by name and type. A shared parameter whose type differs from the pool's gives `D3DERR_INVALIDCALL` from `D3DXCreateEffect`.
- **`D3DXCreateEffect`:**
  - binary data gives the effect;
  - text data exits 4 as plan 7's (Global Constraints);
  - the defines and include are accepted and unused for binaries;
  - the errors buffer is NULL on success.

- [ ] **Step 1: Write the failing tests** (`tests/test_effects.py`):
  - `fx_vtable` (`BW2_GAMEFILES=1`): the slot check above, as a script over the exe's disassembly that prints each called slot and the method name at that slot. Expected: every slot resolves to a method of the right arity.
  - `fx_params`: a synthetic effect from `tests/fxwrite.py` with one of each parameter type, a struct, an array of 4 float4, two annotations and a string. Each Get gives its default. Each Set then Get round-trips. Set by a name string works. `"s.member"` works. A type mismatch is `D3DERR_INVALIDCALL`.
  - `fx_matrix`: row- and column-major matrix parameters, `SetMatrix`/`GetMatrixTranspose` and the pointer arrays.
  - `fx_pool`: two effects in one pool share `float g` when shared and keep their own when not.
  - `fx_text_is_later`: HLSL text data exits 4 with `reached D3DXCreateEffect (HLSL source)`.
  - `fx_game_parse` (`BW2_GAMEFILES=1`): every one of the 59 `.sdv` effects (the binary found by its first dword) parses with no error. The totals are 2,645 parameters, 165 techniques and 172 passes, and every pass state operation is in the table Task 6 builds (printed counts against "Measured").
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_effects.py tests/test_d3dx.py -q` and `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_effects.py -k game -v`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Parse D3DX effects and their parameters"`

### Task 6: Effects: expressions, techniques and passes

**Files:**
- Create: `runtime/bw2/d3dx/fx_expr.c` (the FXLC/CLIT/PRES interpreter), `runtime/bw2/d3dx/fx_states.c` (the state table and its application).
- Modify: `fx_effect.c` (the technique and pass methods), `fx_selftest.c`, `tests/fxwrite.py`, `tests/test_effects.py`.

**Interfaces:**
- Consumes: Task 5's structures; Tasks 3-4's shader objects (created on the device from the effect's shader blobs at first use); plan 4's host-side state setters and state blocks.
- Produces:
  - `int fx_expr_run(const struct fx_expr *x, const struct fx_effect *e, float *out, uint32_t nout);`: float32 arithmetic.
  - `const struct fx_state_info *fx_state(uint32_t op);`: class (render, texture stage, sampler, shader, texture, transform, light, material, constant), the D3D state number, and the value type.
- **The interpreter:** every opcode the 59 effects contain. Step 1's `fx_expr_game_ops` lists them from the files. Any other opcode stops loudly. Inputs are effect parameters (by the PRSI/CTAB mapping), literals (CLIT) and temporaries. Outputs are a state value, an array index, or preshader constants written to a shader's constant registers.
- **The state table:** the fx_2_0 operation numbering (render states first, in D3DRENDERSTATETYPE order from `ZENABLE` = 0, `FILLMODE` = 1, `SHADEMODE` = 2, `ZWRITEENABLE` = 3, `ALPHATESTENABLE` = 4 ...; `VertexShader` = 146 and `PixelShader` = 147, as the pass counts show). Every operation in "Measured" must resolve, which `fx_game_parse` checks.
- **Passes:**
  - `SetTechnique` and `GetCurrentTechnique`.
  - `ValidateTechnique`: every pass's shaders are within the caps, and every state value is legal.
  - `FindNextValidTechnique`: in file order from the given one.
  - `Begin(&passes, flags)` records a state block of everything the technique's passes touch, unless `D3DXFX_DONOTSAVESTATE` (1). Flags 2 and 4 leave shader and sampler state out of it.
  - `BeginPass(i)`:
    1. evaluates expressions;
    2. sets the states, shaders and textures;
    3. uploads each shader's constants from the parameters named in its CTAB, plus its preshader outputs.
  - `CommitChanges` re-uploads what changed since the last upload (by dirty serial).
  - `EndPass`; `End` applies the saved block.
  - `OnLostDevice`/`OnResetDevice` give `D3D_OK`.
  - `IsParameterUsed`: from the CTABs and expressions.
  - `SetStateManager`, parameter blocks and `CloneEffect` stop loudly.

- [ ] **Step 1: Write the failing tests:**
  - `fx_expr`: synthetic expressions `a*b+c`, `dot`, `rcp`, `rsq`, `min`/`max`, `lt`/`ge`, `frc`, `sin`/`cos` and `neg` give float32 results within 1 ulp of C's.
  - `fx_expr_game_ops` (`BW2_GAMEFILES=1`): lists every FXLC opcode in the 59 effects, and each is implemented.
  - `fx_pass_states`: a synthetic technique with two passes setting `ZENABLE`, `ALPHABLENDENABLE`, `SRCBLEND` and a sampler's `MINFILTER`. After `BeginPass(1)`, the device's `GetRenderState`/`GetSamplerState` give the values, and a draw shows the blend.
  - `fx_shaders`: a pass with an assembled vs_1_1 and ps_2_0 whose CTAB names `WorldViewProj` and `Tint`. `SetMatrix`/`SetVector` then `BeginPass` draws the tinted quad.
  - `fx_preshader_commit`: a preshader computing `c0 = Tint * Scale`. Setting `Scale` then `CommitChanges` changes the pixel. With no change, no constant upload is counted.
  - `fx_save_restore`: render and sampler states and the shaders around `Begin`/`End`, with and without `D3DXFX_DONOTSAVESTATE`.
  - `fx_validate`: a technique with a vs_3_0 blob is invalid, and `FindNextValidTechnique` skips it.
  - `fx_game_render` (`BW2_GAMEFILES=1`): for each of the 59 effects and each technique `ValidateTechnique` accepts:
    - `Begin`, then every `BeginPass`/`EndPass` with a dummy quad bound by the pass's declared inputs, then `End`;
    - with no loud stop, and every pass's pipeline compiled.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_effects.py tests/test_shaders.py tests/test_d3d9.py -q` and `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_effects.py -k game -v`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Run D3DX effect techniques and passes"`

### Task 7: Boot to the next layer

**Files:**
- Create: `docs/research/shaders.md`.
- Modify:
  - `tests/test_boot.py`: `test_boot_reaches_effects` becomes `test_boot_after_effects`;
  - the spec, sections 5.2 (plan 5 notes, the phasing, plan 7's names) and 7.3 (what the translator pins);
  - `docs/research/graphics.md` ("For the next plans" points to `shaders.md`).

**Interfaces:**
- Consumes everything above.
- Produces the measured record plan 6 (or 7) starts from.

- [ ] **Step 1: Write the failing test.** `test_boot_after_effects` runs `out/bw2` headless on the game tree with a fresh `BW2_SUPPORT` and asserts:
  - exit code 4;
  - `reached D3DXCreateEffect` is not in stderr;
  - stderr has no `TRIPWIRE`, `trap`, `unresolved`, `unimplemented`, `not implemented` or `uncaught`.
- [ ] **Step 2: Run it.** `.venv/bin/python -m bw2.pipeline --build-only && BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -v`. Expected: PASS once the game's effect calls are answered. Otherwise FAIL at the first one that is not.
- [ ] **Step 3: Bring-up loop.** Run `BW2_HEADLESS=1 BW2_TRACE_IMPORTS=1 out/bw2 --game gamefiles/game`.
  - Each loud stop in this plan's layer (shaders, assembler, effects) or plan 4's is fixed with a selftest check added to the owning group first, red then green.
  - A stop at plan 7's names, or at DirectSound/OpenAL or Bink, is the expected end.
  - Each stop outside every plan's layer, and each tripwire, stops the run and goes to the user.
  - If the game reads input (`GetDeviceData`, `PeekMessageA`) before the stop, record it: plan 6 starts from there.
  - Record each fix in the ledger.
- [ ] **Step 4: Measure and write `shaders.md`:**
  - the effects created (file names, sizes, pool or not) and the techniques chosen;
  - the `ID3DXEffect` methods called, with counts;
  - the shaders created outside effects (assembler or bytecode) and their versions;
  - the pipelines compiled and the time spent compiling, cold and with a warm cache (`<support>/ShaderCache/`);
  - the first frame presented, if any (a headless readback of its size and a non-black check, nothing saved);
  - input reads, if any;
  - the time to the stop;
  - the stop itself.
- [ ] **Step 5: Run the full suite.** `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan5-suite.txt 2>&1; tail -3 .superpowers/sdd/plan5-suite.txt`. Expected: all pass.
- [ ] **Step 6: Audit and commit.** `third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Boot through the game's effects"`
