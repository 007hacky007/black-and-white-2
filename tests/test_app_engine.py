"""The engine as the app's executable (plan 13 Task 4): inside `<X>.app/Contents/MacOS` it finds its game and the
lifted DLL in the bundle when started with no arguments, writes its output to a log in the support folder, and a
watcher (the same binary, `--log-watcher`) shows an alert when the run ends in a loud stop.

Most tests use a small synthetic guest linked with the engine host (tests/fixtures.py guest_engine), so they need
no game files; the boots of the real game carry the gamefiles marker. Every run has a timeout, so a pipe end leaked
into the watcher fails as a timeout instead of hanging the suite."""
import os
import shutil
import signal
import subprocess

import pytest

from bw2 import paths
from fixtures import CODE, asm32, guest_engine, iat_va, make_pe, make_test_bundle

ENGINE = paths.REPO / "out" / "bw2"
QUIT_LINE = "[bw2] quit after 300 input reads (BW2_QUIT_AFTER_READS)"
BASE = 0x401000
K32 = "KERNEL32.dll"


def _exit_guest(code: int) -> bytes:
    imports = {K32: ["ExitProcess"]}
    pad = [(".text", b"\xcc" * 0x200, CODE)]
    asm = f"push {code}\ncall dword ptr [{iat_va(pad, imports, K32, 'ExitProcess'):#x}]\nret"
    return make_pe([(".text", asm32(asm, base=BASE).ljust(0x200, b"\xcc"), CODE)], imports, 0x1000)


@pytest.fixture(scope="module")
def engine(tmp_path_factory):
    """An engine whose white.exe (beside it in game/) calls ExitProcess(0)."""
    d = tmp_path_factory.mktemp("app-engine")
    return guest_engine(d, _exit_guest(0), [BASE], exe_name="white.exe")


def _env(support=None, **extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("BW2_")}
    env.update(BW2_HEADLESS="1", BW2_NO_ALERT="1", **extra)
    if support is not None:
        env["BW2_SUPPORT"] = str(support)
    return env


def _run(binary, *args, env, input=None, timeout=60, cwd="/"):
    return subprocess.run([str(binary), *args], input=input, capture_output=True, timeout=timeout, env=env, cwd=cwd)


def _alerts(stdout: bytes):
    return [ln[len("[bw2] alert: "):] for ln in stdout.decode("utf-8", "replace").splitlines()
            if ln.startswith("[bw2] alert: ")]


# ---- the watcher's rule ------------------------------------------------------

LATER = "[bw2] reached X: owned by a later plan (from guest 0x1, sub_00000001)"
FATAL = "[bw2] FATAL: unresolved direct call at 0x00401000 (in sub_00401000)"
RULE_CASES = [
    ("[guest] a\n[win32hle] ExitProcess(0)\n", None),
    (QUIT_LINE + "\n", None),
    ("[win32hle] ExitProcess(0)\n[guest] b\n", None),
    (LATER + "\n", LATER),
    ("TRIPWIRE: CreateFileA(...)\n", "TRIPWIRE: CreateFileA(...)"),
    (FATAL + "\n", FATAL),
    ("[bw2] DirectInput mouse: first read\n", "[bw2] DirectInput mouse: first read"),
    ("[win32hle] ExitProcess(1)\n", "[win32hle] ExitProcess(1)"),
    ("", "no message"),
    # beyond the plan's list: the input script's quit, the movie quit, a guest line with no newline before an
    # engine line, a boot failure followed by usage, no final newline, win32hle's own fatal, guest lines only
    ("[bw2] script: quit at read 812\n", None),
    ("[bw2] quit at movie frame 1:2 + 3 presents (BW2_QUIT_AFTER_MOVIE)\n", None),
    ("[guest] text" + QUIT_LINE + "\n", None),
    ("[guest] x" + LATER + "\n", LATER),
    ("bw2: no game folder /x\nusage: bw2 --game DIR\n", "bw2: no game folder /x"),
    ("[win32hle] ExitProcess(0)", None),
    (FATAL, FATAL),
    ("[win32hle] FATAL: cannot read the PE header of x\n", "[win32hle] FATAL: cannot read the PE header of x"),
    ("[guest] only the game spoke\n", "no message"),
]


@pytest.mark.parametrize("text,alert", RULE_CASES)
def test_log_watcher_rule(engine, tmp_path, text, alert):
    log = tmp_path / "bw2.log"
    r = _run(engine, "--log-watcher", str(log), env=_env(), input=text.encode())
    assert r.returncode == 0, r.stderr
    assert _alerts(r.stdout) == ([] if alert is None else [alert]), r.stdout
    assert log.read_bytes() == text.encode()


# ---- where the engine finds its files -------------------------------------------

def test_selftest_app_outside_bundle(engine):
    r = _run(engine, "--selftest-app", env=_env())
    assert r.returncode == 0, r.stderr
    lines = r.stdout.decode().splitlines()
    assert lines[0] == "bundle 0", lines
    assert f"d3dx {engine.resolve().parent}/d3dx9_25.dll" in lines, lines


@pytest.mark.gamefiles
def test_selftest_app_out_engine():
    r = _run(ENGINE, "--selftest-app", env=_env())
    assert r.returncode == 0, r.stderr
    lines = r.stdout.decode().splitlines()
    assert lines[0] == "bundle 0" and f"d3dx {paths.REPO}/out/d3dx9_25.dll" in lines, lines


def test_selftest_app_in_bundle(engine, tmp_path):
    app = make_test_bundle(tmp_path, None, engine=engine, dll=None).resolve()
    r = _run(app / "Contents/MacOS/bw2", "--selftest-app", env=_env(tmp_path / "support"))
    assert r.returncode == 0, r.stderr
    assert r.stdout.decode().splitlines() == [f"bundle 1 {app}/Contents", f"game {app}/Contents/Resources/game",
                                              f"d3dx {app}/Contents/Resources/d3dx9_25.dll"]
    assert not (tmp_path / "support").exists()       # the selftest needs no log


# ---- loud stops in a bundle reach the log and the alert ----------------------------

def _bundle_run(app, support, *args, **extra):
    return _run(app / "Contents/MacOS/bw2", *args, env=_env(support, **extra))


def _log(support):
    return (support / "Logs" / "bw2.log").read_text(errors="replace").splitlines()


def test_bundle_loud_stop_alerts(engine, tmp_path):
    app = make_test_bundle(tmp_path, None, engine=engine, dll=None)
    support = tmp_path / "support"
    r = _bundle_run(app, support)
    assert r.returncode == 2, r
    assert r.stderr == b""
    fails = [ln for ln in _log(support) if ln.startswith("bw2: ")]
    assert len(fails) == 1 and "white.exe" in fails[0], _log(support)
    assert _alerts(r.stdout) == fails


def test_bundle_missing_game_alerts(engine, tmp_path):
    app = make_test_bundle(tmp_path, None, engine=engine, dll=None)
    (app / "Contents/Resources/game").rmdir()
    support = tmp_path / "support"
    r = _bundle_run(app, support)
    assert r.returncode == 2, r
    assert r.stderr == b""
    fails = [ln for ln in _log(support) if ln.startswith("bw2: no game folder ")]
    assert len(fails) == 1 and fails[0].endswith("/Contents/Resources/game"), _log(support)
    assert _alerts(r.stdout) == fails


def test_bundle_bad_argument_alerts(engine, tmp_path):
    app = make_test_bundle(tmp_path, None, engine=engine, dll=None)
    support = tmp_path / "support"
    r = _bundle_run(app, support, "--bogus")
    assert r.returncode == 2, r
    assert r.stderr == b""
    assert "bw2: unknown argument --bogus" in _log(support)
    assert _alerts(r.stdout) == ["bw2: unknown argument --bogus"]


def test_bundle_skips_psn_argument(engine, tmp_path):
    # LaunchServices on older systems passes -psn_<n>_<m>: it is not an unknown argument
    app = make_test_bundle(tmp_path, None, engine=engine, dll=None)
    shutil.copy2(engine.parent / "game" / "white.exe", app / "Contents/Resources/game/white.exe")
    support = tmp_path / "support"
    r = _bundle_run(app, support, "-psn_0_12345")
    assert r.returncode == 0, _log(support)
    assert _log(support)[-1] == "[win32hle] ExitProcess(0)"


def test_watcher_gone(engine, tmp_path):
    app = make_test_bundle(tmp_path, None, engine=engine, dll=None)
    r = _bundle_run(app, tmp_path / "support", BW2_TEST_KILL_WATCHER="1")
    assert r.returncode == 2, (r.returncode, -signal.SIGPIPE)
    assert _alerts(r.stdout) == []


# ---- a whole run in a bundle ----------------------------------------------------------

def test_bundle_guest_runs_no_args(engine, tmp_path):
    # the synthetic guest as the bundle's game: no arguments, from /, a clean exit, the log, no alert; a second run
    # keeps the first log as bw2.previous.log; with BW2_LOG=stderr the output stays on stderr and no log is made
    app = make_test_bundle(tmp_path, engine.parent / "game", engine=engine, dll=None)
    support = tmp_path / "support"
    r = _bundle_run(app, support)
    assert r.returncode == 0, r
    assert r.stderr == b"" and _alerts(r.stdout) == []
    first = (support / "Logs" / "bw2.log").read_bytes()
    assert first.decode().splitlines()[-1] == "[win32hle] ExitProcess(0)"
    r = _bundle_run(app, support)
    assert r.returncode == 0, r
    assert (support / "Logs" / "bw2.previous.log").read_bytes() == first
    other = tmp_path / "support2"
    r = _bundle_run(app, other, BW2_LOG="stderr")
    assert r.returncode == 0, r
    assert r.stderr.decode().splitlines()[-1] == "[win32hle] ExitProcess(0)"
    assert not (other / "Logs").exists()


def test_bundle_guest_loud_exit_alerts(tmp_path):
    b = guest_engine(tmp_path, _exit_guest(3), [BASE], exe_name="white.exe")
    app = make_test_bundle(tmp_path / "app", tmp_path / "game", engine=b, dll=None)
    support = tmp_path / "support"
    r = _bundle_run(app, support)
    assert r.returncode == 3, r
    assert _alerts(r.stdout) == ["[win32hle] ExitProcess(3)"]


@pytest.mark.gamefiles
def test_bundle_boot_no_args(tmp_path):
    app = make_test_bundle(tmp_path, paths.GAMEFILES / "game")
    support = tmp_path / "support"
    runs = []
    for _ in range(2):
        r = _run(app / "Contents/MacOS/bw2", env=_env(support, BW2_QUIT_AFTER_READS="300"), timeout=300)
        assert r.returncode == 0, _log(support)[-20:]
        assert r.stderr == b"" and _alerts(r.stdout) == [], r.stdout[-2000:]
        assert _log(support)[-1] == QUIT_LINE, _log(support)[-20:]
        runs.append((support / "Logs" / "bw2.log").read_bytes())
    assert (support / "Logs" / "bw2.previous.log").read_bytes() == runs[0]
    other = tmp_path / "support2"
    r = _run(app / "Contents/MacOS/bw2", env=_env(other, BW2_QUIT_AFTER_READS="300", BW2_LOG="stderr"), timeout=300)
    assert r.returncode == 0, r.stderr[-2000:]
    assert QUIT_LINE in r.stderr.decode(errors="replace").splitlines()
    assert not (other / "Logs").exists()


@pytest.mark.gamefiles
@pytest.mark.skipif(os.environ.get("BW2_GUI") != "1", reason="opens the game's window: set BW2_GUI=1")
def test_bundle_opens_from_finder(tmp_path):
    app = make_test_bundle(tmp_path, paths.GAMEFILES / "game")
    support = tmp_path / "support"
    r = subprocess.run(["/usr/bin/open", "-W", "-n", "--env", "BW2_QUIT_AFTER_READS=300", "--env",
                        f"BW2_SUPPORT={support}", "--env", "BW2_NO_ALERT=1", str(app)],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr
    assert _log(support)[-1] == QUIT_LINE, _log(support)[-20:]
