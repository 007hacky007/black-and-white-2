"""BW2 Builder.app (plan 13 Task 8): making it with bw2.make_builder, its checklist printed without a window
(`--print-checks`), and the window's calls run end to end by XCTest (opt-in, builder_e2e).

The window itself is not clicked by a test (plan 13 Ruling 14): its logic is BuilderCore's, unit-tested by
tests/test_builder_swift.py, and `--print-checks` evaluates the same rows the window shows."""
import json
import os
import plistlib
import pwd
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from bw2 import make_builder as mb
from bw2 import paths
from bw2.app import version
from bw2.workspace import SOURCES, source_list

from test_builder_swift import xctest_skip_reason

REPO = paths.REPO
FIXED_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
CHECK_IDS = ["mac", "commandLineTools", "buildTools", "discs", "patch11", "patch12", "fanPatch", "destination",
             "diskSpace"]
FORBIDDEN = (".exe", ".dll", ".bin", ".iso", ".cue", ".zip")
DISC = "Black & White 2 (USA) (Disc {})"


def _swift_or_skip() -> Path:
    swift = mb.find_swift()
    if swift is None:
        pytest.skip(f"needs swift under xcode-select -p ({mb.developer_dir() or 'nothing selected'})")
    return swift


@pytest.fixture(scope="module")
def builder_app(tmp_path_factory) -> Path:
    """BW2 Builder.app made once for this module, in a folder whose name has a space and an ampersand."""
    _swift_or_skip()
    out = tmp_path_factory.mktemp("builder") / "a & b"
    return mb.make_builder(out=out)


def _real_support() -> Path:
    return Path(pwd.getpwuid(os.getuid()).pw_dir) / "Library" / "Application Support" / "BW2 Builder"


def _snapshot(p: Path):
    if not p.exists():
        return None
    return (p.stat().st_mtime_ns, sorted((c.name, c.stat(follow_symlinks=False).st_mtime_ns)
                                      for c in os.scandir(p)))


# ---- pieces that need no swift -----------------------------------------------------------------------------------

def test_info_plist():
    assert mb.info_plist("1.2.3") == {
        "CFBundleExecutable": "BW2Builder",
        "CFBundleIdentifier": "local.bw2-port.builder",
        "CFBundleName": "BW2 Builder",
        "CFBundleDisplayName": "BW2 Builder",
        "CFBundleIconFile": "AppIcon.icns",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.2.3",
        "CFBundleVersion": "1.2.3",
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": "13.0",
        "LSArchitecturePriority": ["arm64"],
        "NSHighResolutionCapable": True,
        "BW2BuilderVersion": "1.2.3",
    }


def test_find_swift_under_developer_dir(tmp_path, monkeypatch):
    """swift is looked up by file under xcode-select -p, never through the /usr/bin shim."""
    dev = tmp_path / "Developer"
    monkeypatch.setattr(mb, "developer_dir", lambda: dev)
    assert mb.find_swift() is None
    tool = dev / "Toolchains" / "XcodeDefault.xctoolchain" / "usr" / "bin" / "swift"
    tool.parent.mkdir(parents=True)
    tool.write_text("")
    tool.chmod(0o755)
    assert mb.find_swift() == tool
    clt = dev / "usr" / "bin" / "swift"
    clt.parent.mkdir(parents=True)
    clt.write_text("")
    clt.chmod(0o755)
    assert mb.find_swift() == clt
    monkeypatch.setattr(mb, "developer_dir", lambda: None)
    assert mb.find_swift() is None


def test_make_builder_refuses_without_swift(tmp_path, monkeypatch):
    monkeypatch.setattr(mb, "find_swift", lambda: None)
    with pytest.raises(mb.MakeError, match="swift"):
        mb.make_builder(out=tmp_path)
    assert list(tmp_path.iterdir()) == []


def test_make_builder_names_a_failed_build(tmp_path, monkeypatch):
    """A failed swift build is named with its output tail, and nothing is left in out."""
    fake = tmp_path / "swift"
    fake.write_text("#!/bin/sh\necho 'error: something broke' >&2\nexit 1\n")
    fake.chmod(0o755)
    out = tmp_path / "out"
    monkeypatch.setattr(mb, "find_swift", lambda: fake)
    with pytest.raises(mb.MakeError, match="something broke"):
        mb.make_builder(out=out)
    assert not out.exists() or list(out.iterdir()) == []


def test_make_builder_names_failed_icon_and_keeps_existing_app(tmp_path, monkeypatch):
    exe = tmp_path / "BW2Builder"
    exe.write_bytes(b"test executable")
    out = tmp_path / "out"
    old = out / mb.APP
    old.mkdir(parents=True)
    (old / "keep").write_text("existing app")
    monkeypatch.setattr(mb, "find_swift", lambda: exe)
    monkeypatch.setattr(mb, "swift_build", lambda swift: exe)
    monkeypatch.setattr(mb, "copy_sources", lambda dest, **kw: dest.mkdir(parents=True))

    def fail_icon(png, destination):
        raise subprocess.CalledProcessError(1, ["iconutil"], stderr=b"invalid iconset")

    monkeypatch.setattr(mb, "make_icns", fail_icon)
    with pytest.raises(mb.MakeError, match="icon.*invalid iconset"):
        mb.make_builder(out=out)
    assert (old / "keep").read_text() == "existing app"
    assert sorted(p.name for p in out.iterdir()) == [mb.APP]


# ---- the made app ------------------------------------------------------------------------------------------------

def test_make_builder(builder_app):
    app = builder_app
    assert app.name == "BW2 Builder.app"
    assert not app.with_name(app.name + ".part").exists()
    r = subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    contents = app / "Contents"
    assert sorted(p.name for p in (contents / "MacOS").iterdir()) == ["BW2Builder"]
    source = contents / "Resources" / "source"
    got = sorted(str(p.relative_to(source)) for p in source.rglob("*") if p.is_file() or p.is_symlink())
    assert got == sorted(source_list(REPO) + [SOURCES])
    assert (source / SOURCES).read_text().splitlines() == source_list(REPO)
    bad = []
    for p in app.rglob("*"):
        rel = p.relative_to(app)
        parts = rel.parts
        if {"gamefiles", "gen", "out"} & set(parts):
            bad.append(str(rel))
        elif p.suffix.lower() in FORBIDDEN:
            bad.append(str(rel))
        elif p.suffix.lower() == ".c" and parts[:4] not in (("Contents", "Resources", "source", "runtime"),
                                                           ("Contents", "Resources", "source", "third_party")):
            bad.append(str(rel))
    assert bad == []
    info = plistlib.loads((contents / "Info.plist").read_bytes())
    assert info == mb.info_plist(version())
    icon = contents / "Resources" / info["CFBundleIconFile"]
    data = icon.read_bytes()
    assert data[:4] == b"icns"
    assert int.from_bytes(data[4:8], "big") == len(data)
    assert info["BW2BuilderVersion"] == (REPO / "VERSION").read_text().strip()


def test_builder_print_checks(builder_app, tmp_path):
    real = _real_support()
    before = _snapshot(real)
    support = tmp_path / "support"
    env = {"PATH": FIXED_PATH, "HOME": str(tmp_path), "CFFIXED_USER_HOME": str(tmp_path)}
    exe = builder_app / "Contents" / "MacOS" / "BW2Builder"
    r = subprocess.run([str(exe), "--support", str(support), "--print-checks"], env=env, cwd="/",
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stdout + r.stderr
    rows = [json.loads(line) for line in r.stdout.splitlines()]
    assert [row["id"] for row in rows] == CHECK_IDS, r.stdout
    assert all(set(row) == {"id", "state", "text"} and row["text"] for row in rows), rows
    state = {row["id"]: row["state"] for row in rows}
    assert state["mac"] == "ready", rows
    for rid in ("discs", "patch11", "patch12", "fanPatch", "buildTools"):
        assert state[rid] == "blocked", rows
    assert _snapshot(real) == before


def test_builder_print_checks_saved_choices(builder_app, tmp_path):
    """The rows use the inputs saved in the support folder: a wrong file is named in its row."""
    support = tmp_path / "support"
    support.mkdir()
    wrong = tmp_path / "BW2Patch1_1.exe"
    wrong.write_bytes(b"not a patch")
    (support / "choices.json").write_text(json.dumps({"patch11": str(wrong), "discs": [str(tmp_path / "nope")]}))
    env = {"PATH": FIXED_PATH, "HOME": str(tmp_path), "CFFIXED_USER_HOME": str(tmp_path)}
    exe = builder_app / "Contents" / "MacOS" / "BW2Builder"
    r = subprocess.run([str(exe), "--support", str(support), "--print-checks"], env=env, cwd="/",
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stdout + r.stderr
    rows = {row["id"]: row for row in map(json.loads, r.stdout.splitlines())}
    assert rows["patch11"] == {"id": "patch11", "state": "blocked", "text": "Not patch 1.1: BW2Patch1_1.exe"}
    assert rows["discs"]["state"] == "blocked"
    assert f"not found: {tmp_path / 'nope'}" in rows["discs"]["text"]


def test_builder_bad_argument(builder_app, tmp_path):
    exe = builder_app / "Contents" / "MacOS" / "BW2Builder"
    r = subprocess.run([str(exe), "--bogus"], env={"PATH": FIXED_PATH, "HOME": str(tmp_path)},
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 2
    assert "--bogus" in r.stderr
    r = subprocess.run([str(exe), "--support"], env={"PATH": FIXED_PATH, "HOME": str(tmp_path)},
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 2
    assert "--support" in r.stderr


# ---- the window's calls, end to end ------------------------------------------------------------------------------

@pytest.mark.gamefiles
@pytest.mark.builder_e2e
def test_builder_e2e_window_calls(builder_app, tmp_path):
    reason = xctest_skip_reason()
    if reason:
        pytest.skip(reason)
    zips = [paths.discs_dir() / f"{DISC.format(n)}.zip" for n in range(1, 5)]
    pdir = paths.GAMEFILES / "patches"
    patches = [pdir / "BW2Patch1_1.exe", pdir / "BW2Patch1_2.exe", pdir / "B&W2 Fan Patch v1.42 Installer.exe"]
    for p in zips + patches:
        assert p.is_file(), p
    env = dict(os.environ)
    env["BW2_E2E_INPUTS"] = ":".join(map(str, zips + patches))
    env["BW2_E2E_SOURCES"] = str(builder_app / "Contents" / "Resources" / "source")
    t0 = time.monotonic()
    proc = subprocess.Popen(["swift", "test", "--package-path", str(REPO / "builder"), "--filter", "EndToEndTests"],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    timer = threading.Timer(3600, proc.kill)
    timer.start()
    tail = []
    try:
        for line in proc.stdout:
            print(f"[e2e +{time.monotonic() - t0:7.1f} s] {line}", end="", flush=True)
            tail = (tail + [line])[-80:]
        rc = proc.wait()
    finally:
        timer.cancel()
    text = "".join(tail)
    assert rc == 0, text
    # The last mark is printed only when every call ran (a skip throws before any mark).
    assert "E2E" in text and "installed and verified" in text, text
