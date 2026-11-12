"""Plan 9 Tasks 4-5: the lifted d3dx9_25.dll, loaded lazily beside the engine; D3DXCompileShader, HLSL effects,
the fragment linker and D3DXCreateSphere through it.

Each test runs the engine's --check-d3dx harness (runtime/bw2/d3dx/d3dx_check.c) after the game's boot setup:
the check names go in on stdin, one per line, and each prints "<check>: ok" or "<check>: FAIL <why>". Every
HLSL and assembly text here is written for the test into the check folder; nothing from the game is read or
written. The lifted DLL comes from the user's disc 1 (Task 1), so every test needs the game files."""
import os
import re
import shutil
import subprocess

import pytest

from bw2 import paths

ENGINE = paths.REPO / "out" / "bw2"
LOADER = re.compile(r"^\[bw2\] d3dx9_25\.dll: lifted DLL at 0x08000000, (\d+) imports unanswered$", re.M)

PS = "float4 c; float4 main(float4 t : TEXCOORD0) : COLOR { return t * c; }\n"
VS = "float4x4 m; float4 main(float4 p : POSITION) : POSITION { return mul(p, m); }\n"
ERR = "float4 main() : COLOR { return undefined_thing; }\n"
ASM = {
    "ps11.asm": "ps_1_1\ntex t0\nmul r0, t0, v0\n",
    "ps20.asm": "ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n",
    "vs11.asm": "vs_1_1\ndcl_position v0\ndp4 oPos.x, v0, c0\ndp4 oPos.y, v0, c1\ndp4 oPos.z, v0, c2\n"
                "dp4 oPos.w, v0, c3\n",
}
# Task 5: effects in HLSL text. Line numbers matter for EFFECT_ERROR (its line 3 is broken).
EFFECT_BODY = ("texture tex;\n"
               "sampler s = sampler_state { Texture = <tex>; MinFilter = Linear; AddressU = Clamp; };\n"
               "float4 k = {1, 0.5, 0.25, 1};\n"
               "float4 vs(float4 p : POSITION) : POSITION { return p; }\n"
               "float4 ps(float2 t : TEXCOORD0) : COLOR { return tex2D(s, t) * k; }\n")
EFFECT = EFFECT_BODY + ("technique T { pass P { ZEnable = false; VertexShader = compile vs_2_0 vs();\n"
                        "    PixelShader = compile ps_2_0 ps(); } }\n")
EFFECT_SB = EFFECT_BODY + ("technique T { pass P {\n"
                           "    StateBlock = stateblock_state { ZWriteEnable = false; AlphaBlendEnable = false; };\n"
                           "    VertexShader = compile vs_2_0 vs(); PixelShader = compile ps_2_0 ps(); } }\n")
EFFECT_POOL_A = "shared float4 g = {1, 2, 3, 4};\nfloat4 a = {9, 9, 9, 9};\ntechnique T { pass P { } }\n"
EFFECT_POOL_B = "shared float4 g;\nfloat b;\ntechnique U { pass Q { } }\n"
EFFECT_ERROR = ("float4 k;\n"
                "float4 ps() : COLOR { return k; }\n"
                "float4 broken( { return k; }\n"
                "technique T { pass P { PixelShader = compile ps_2_0 ps(); } }\n")
FRAGMENTS = ("void f_pos(float4 p : POSITION, out float4 o : POSITION) { o = p; }\n"
             "void f_col(float4 c : COLOR0, out float4 oc : COLOR0) { oc = c; }\n"
             "vertexfragment pos = compile_fragment vs_1_1 f_pos();\n"
             "vertexfragment col = compile_fragment vs_1_1 f_col();\n")
FROM_FILE = "hlsl_from_file.hlsl"          # in the support folder's C/, read by the check as C:\hlsl_from_file.hlsl

# Measured in Step 4 (docs/research/hlsl.md):
UNANSWERED_COUNT = 52                      # the loader line's count
CPU_SELECTION = 2                          # [0x0821129C] after one lifted D3DXMatrixMultiply: SSE2
DEBUGSETMUTE_CALLS = 0                     # DebugSetMute calls in one d3dx_validator run (none)
VALIDATOR_INSTRUCTIONS = 5                 # Instruction (slot 4) calls for the ps_2_0 shader, flags 0

# The imports the lifted D3DX may leave unanswered (Microsoft API names only). Each stops loudly by name if
# it is ever called.
ALLOWED_UNANSWERED = {
    # msvcrt names Task 3 left unanswered on purpose: files, wide ctype, time strings, setjmp, C++ EH, exit
    "fopen", "_wfopen", "fread", "fwrite", "fclose", "_tempnam", "sscanf", "rand", "srand", "_ultoa", "wcstombs",
    "iswspace", "iswalpha", "iswdigit", "iswpunct", "_strtime", "_strdate", "_setjmp3", "longjmp",
    "_except_handler3", "__CxxFrameHandler", "exit",
    # kernel32 names the five entry points do not reach
    "ExpandEnvironmentStringsA", "DeleteFileW", "MoveFileW", "GetTempFileNameW", "FindResourceW",
    # GDI32: ID3DXFont and text meshes, which stay ours (plan 7); BW2 has no GDI device-context layer
    "GetCurrentObject", "SetMapMode", "GetObjectW", "GetObjectA", "ExtTextOutW", "ExtTextOutA", "MoveToEx",
    "CreateCompatibleDC", "DeleteDC", "SelectObject", "DeleteObject", "SetTextAlign", "SetBkMode", "SetBkColor",
    "SetTextColor", "CreateFontIndirectW", "CreateFontIndirectA", "GetFontLanguageInfo", "GetOutlineTextMetricsA",
    "GetCharacterPlacementW", "GetCharacterPlacementA", "GetGlyphOutlineA", "GetGlyphOutlineW", "GetTextMetricsA",
    "GetTextMetricsW",
}


def write_inputs(d):
    d.mkdir(parents=True, exist_ok=True)
    (d / "ps.hlsl").write_text(PS)
    (d / "vs.hlsl").write_text(VS)
    (d / "error.hlsl").write_text(ERR)
    for name, text in ASM.items():
        (d / name).write_text(text)
    for name, text in (("effect.fx", EFFECT), ("effect_sb.fx", EFFECT_SB), ("pool_a.fx", EFFECT_POOL_A),
                       ("pool_b.fx", EFFECT_POOL_B), ("effect_error.fx", EFFECT_ERROR),
                       ("fragments.fx", FRAGMENTS)):
        (d / name).write_text(text)


def run_checks(tmp_path, checks, engine=ENGINE, extra_env=None, timeout=600):
    assert engine.exists(), "build it first: python -m bw2.pipeline"
    d = tmp_path / "in"
    write_inputs(d)
    support = tmp_path / "support"
    (support / "C").mkdir(parents=True, exist_ok=True)
    (support / "C" / FROM_FILE).write_text(PS)
    env = dict(os.environ, BW2_SUPPORT=str(support), BW2_HEADLESS="1", **(extra_env or {}))
    for k in ("BW2_TRACE_IMPORTS", "BW2_QUIT_AFTER_READS", "BW2_INPUT_SCRIPT"):
        if k not in (extra_env or {}):
            env.pop(k, None)
    r = subprocess.run([str(engine), "--game", str(paths.GAMEFILES / "game"), "--check-d3dx", str(d)],
                       input="".join(c + "\n" for c in checks), capture_output=True, text=True, timeout=timeout,
                       env=env)
    (tmp_path / "stdout.txt").write_text(r.stdout)
    (tmp_path / "stderr.txt").write_text(r.stderr)
    return r


def passed(r, *checks):
    tail = r.stdout[-4000:] + "\n--- stderr\n" + r.stderr[-4000:]
    for c in checks:
        assert f"{c}: ok" in r.stdout.splitlines(), tail
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    for loud in ("[module] GetProcAddress(", "LoadLibraryA(\"d3d9.dll\"): not found", "FATAL", "trap:",
                 "unresolved indirect call", "unimplemented import"):
        assert loud not in r.stderr, f"{loud!r} in stderr\n{tail}"


def info(r, check, key):
    """The value a check printed as "<check>: <key> <value>"."""
    m = re.search(rf"^{check}: {key} (.*)$", r.stdout, re.M)
    assert m, f"no {key} from {check}\n{r.stdout[-3000:]}"
    return m.group(1)


@pytest.mark.gamefiles
def test_d3dx_load(tmp_path):
    # the first lookup loads the DLL (its DllMain runs: DisableThreadLibraryCalls, the _adjust_fdiv read) and
    # prints the loader line; the second prints nothing
    r = run_checks(tmp_path, ["d3dx_load"])
    passed(r, "d3dx_load")
    loads = LOADER.findall(r.stderr)
    assert len(loads) == 1, r.stderr[-3000:]
    assert int(loads[0]) == UNANSWERED_COUNT, loads
    names = info(r, "d3dx_load", "unanswered").split()
    assert len(names) == UNANSWERED_COUNT, names
    assert set(names) <= ALLOWED_UNANSWERED, sorted(set(names) - ALLOWED_UNANSWERED)


@pytest.mark.gamefiles
def test_d3dx_data_import(tmp_path):
    passed(run_checks(tmp_path, ["d3dx_data_import"]), "d3dx_data_import")


@pytest.mark.gamefiles
def test_d3dx_module_name(tmp_path):
    passed(run_checks(tmp_path, ["d3dx_module_name"]), "d3dx_module_name")


@pytest.mark.gamefiles
def test_d3dx_validator(tmp_path):
    r = run_checks(tmp_path, ["d3dx_validator"])
    passed(r, "d3dx_validator")
    assert info(r, "d3dx_validator", "flags0") == f"created 1 begin 1 instruction {VALIDATOR_INSTRUCTIONS} end 1 release 1"
    assert info(r, "d3dx_validator", "skip") == "created 0"
    # the compile ran the lookup site in sub_08149A1B (entered by a tail jump from sub_0815B4B9)
    assert info(r, "d3dx_validator", "begin_from") == "sub_0815B4B9"
    assert info(r, "d3dx_validator", "instruction_from") == "sub_08143905"
    # with SKIPVALIDATION only the CTAB's flags dword differs
    assert info(r, "d3dx_validator", "skip_tokens") == "46 and 46, 12 without comments, 1 differ"
    assert int(info(r, "d3dx_validator", "debugsetmute")) == DEBUGSETMUTE_CALLS


@pytest.mark.gamefiles
def test_d3dx_cpu(tmp_path):
    r = run_checks(tmp_path, ["d3dx_cpu"])
    passed(r, "d3dx_cpu")
    assert int(info(r, "d3dx_cpu", "selection"), 16) == CPU_SELECTION


@pytest.mark.gamefiles
def test_d3dx_missing(tmp_path):
    # the engine alone in a folder: the first HLSL call stops, naming the path it looked at; a run that never
    # needs the lifted DLL does not miss it
    alone = tmp_path / "alone"
    alone.mkdir()
    shutil.copy2(ENGINE, alone / "bw2")
    r = run_checks(tmp_path, ["hlsl_compile_ps"], engine=alone / "bw2")
    assert r.returncode != 0, r.stdout[-2000:]
    line = [ln for ln in r.stderr.splitlines() if ln.startswith("[bw2] FATAL: d3dx9_25.dll (lifted):")]
    assert line and os.path.realpath(alone) in line[0], r.stderr[-3000:]
    r = run_checks(tmp_path, [], engine=alone / "bw2")
    assert r.returncode == 0 and "FATAL" not in r.stderr, r.stderr[-3000:]


@pytest.mark.gamefiles
def test_hlsl_compile_ps(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_compile_ps"]), "hlsl_compile_ps")


@pytest.mark.gamefiles
def test_hlsl_compile_vs(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_compile_vs"]), "hlsl_compile_vs")


@pytest.mark.gamefiles
def test_hlsl_compile_error(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_compile_error"]), "hlsl_compile_error")


@pytest.mark.gamefiles
def test_hlsl_asm_matches_native(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_asm_matches_native"]), "hlsl_asm_matches_native")


@pytest.mark.gamefiles
def test_hlsl_constant_table(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_constant_table"]), "hlsl_constant_table")


@pytest.mark.gamefiles
def test_hlsl_two_threads(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_two_threads"]), "hlsl_two_threads")


@pytest.mark.gamefiles
def test_hlsl_from_file(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_from_file"]), "hlsl_from_file")


@pytest.mark.gamefiles
def test_hlsl_log(tmp_path):
    # BW2_HLSL_LOG: one line per forwarded compile, numbers and the profile only
    r = run_checks(tmp_path, ["hlsl_compile_ps"], extra_env={"BW2_HLSL_LOG": "1"})
    passed(r, "hlsl_compile_ps")
    logs = [ln for ln in r.stderr.splitlines() if ln.startswith("[bw2] hlsl shader ")]
    assert len(logs) == 1 and re.fullmatch(r"\[bw2\] hlsl shader ps_2_0: \d+ bytes -> 0x00000000, \d+ tokens",
                                           logs[0]), logs
    assert f": {len(PS)} bytes" in logs[0], logs


# ---- Task 5: HLSL effects, the fragment linker and the sphere ----

@pytest.mark.gamefiles
def test_hlsl_effect(tmp_path):
    # D3DXCreateEffect with HLSL text: the lifted effect compiler makes fx_2_0 bytes, plan 5's path makes the
    # effect; its pass sets our device's shaders and states, and End puts them back
    r = run_checks(tmp_path, ["hlsl_effect"])
    passed(r, "hlsl_effect")
    assert info(r, "hlsl_effect", "desc") == "techniques 1 parameters 3"


@pytest.mark.gamefiles
def test_hlsl_effect_stateblock(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_effect_stateblock"]), "hlsl_effect_stateblock")


@pytest.mark.gamefiles
def test_hlsl_effect_pool(tmp_path):
    passed(run_checks(tmp_path, ["hlsl_effect_pool"]), "hlsl_effect_pool")


@pytest.mark.gamefiles
def test_hlsl_effect_error(tmp_path):
    # a broken line 3: a failing HRESULT, no effect, D3DX's file(line,col) message, twice the same (the game
    # asks twice)
    r = run_checks(tmp_path, ["hlsl_effect_error"])
    passed(r, "hlsl_effect_error")
    assert int(info(r, "hlsl_effect_error", "hr"), 16) & 0x80000000
    # D3DX's file(line,col) form; the April 2005 compiler gives a syntax error (X3000) its line alone
    assert info(r, "hlsl_effect_error", "line") == "(3)"


@pytest.mark.gamefiles
def test_hlsl_effect_log(tmp_path):
    r = run_checks(tmp_path, ["hlsl_effect"], extra_env={"BW2_HLSL_LOG": "1"})
    passed(r, "hlsl_effect")
    logs = [ln for ln in r.stderr.splitlines() if ln.startswith("[bw2] hlsl effect: ")]
    assert len(logs) == 1, logs
    assert re.fullmatch(rf"\[bw2\] hlsl effect: {len(EFFECT)} bytes -> 0x00000000, \d+ bytes, 1 techniques, \d+ ms",
                        logs[0]), logs


@pytest.mark.gamefiles
def test_hlsl_fragment_linker(tmp_path):
    r = run_checks(tmp_path, ["hlsl_fragment_linker"])
    passed(r, "hlsl_fragment_linker")
    assert info(r, "hlsl_fragment_linker", "fragments") == "2"


@pytest.mark.gamefiles
def test_hlsl_sphere_draw(tmp_path):
    # D3DXCreateSphere's lifted mesh: 8 slices, 4 stacks; DrawSubset(0) is one DrawIndexedPrimitive on our device
    r = run_checks(tmp_path, ["hlsl_sphere_draw"])
    passed(r, "hlsl_sphere_draw")
    assert info(r, "hlsl_sphere_draw", "mesh") == "vertices 26 faces 48 fvf 0x00000012"
    assert info(r, "hlsl_sphere_draw", "draws") == "1"
