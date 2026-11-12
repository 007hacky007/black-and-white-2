"""Plan 9 Task 4: the lifted d3dx9_25.dll, loaded lazily beside the engine, and D3DXCompileShader through it.

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
