"""The Builder's Swift core (plan 13 Task 7): its XCTest suite, and the numbers and messages it copies from
bw2/builder_cli.py, compared here so the measured steps, weights, space and message texts cannot drift."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from bw2 import builder_cli, paths

REPO = paths.REPO
PACKAGE = REPO / "builder"
CORE = PACKAGE / "Sources" / "BuilderCore"
XCTEST = "Platforms/MacOSX.platform/Developer/Library/Frameworks/XCTest.framework"


def _swift_sources() -> str:
    return "\n".join(p.read_text() for p in sorted(CORE.glob("*.swift")))


def xctest_skip_reason() -> str | None:
    """Why the XCTest suite cannot run here, or None. The Command Line Tools have no XCTest; only Xcode does."""
    try:
        r = subprocess.run(["/usr/bin/xcode-select", "-p"], capture_output=True, text=True, timeout=30)
        dev = r.stdout.strip() if r.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        dev = ""
    if not dev or not (Path(dev) / XCTEST).is_dir():
        return f"needs Xcode with XCTest; xcode-select -p names {dev or 'nothing'}"
    if shutil.which("swift") is None:
        return "needs Xcode with XCTest; no swift found"
    return None


def test_swift_builder_core(tmp_path):
    reason = xctest_skip_reason()
    if reason:
        pytest.skip(reason)
    log = tmp_path / "swift-test.log"
    with open(log, "w") as f:
        r = subprocess.run(["swift", "test", "--package-path", str(PACKAGE)], stdout=f, stderr=subprocess.STDOUT,
                           timeout=600)
    tail = "\n".join(log.read_text(errors="replace").splitlines()[-60:])
    assert r.returncode == 0, tail


def test_steps_agree():
    src = _swift_sources()
    m = re.search(r"public let buildSteps: \[\(id: String, weight: Double\)\] = \[(.*?)\]", src, re.S)
    assert m, "buildSteps not found in builder/Sources/BuilderCore"
    pairs = re.findall(r'\("(\w+)",\s*([\d.]+)\)', m.group(1))
    assert [p[0] for p in pairs] == [sid for sid, _ in builder_cli.STEPS]
    assert {sid: float(w) for sid, w in pairs} == {k: float(v) for k, v in builder_cli.WEIGHTS.items()}

    m = re.search(r"public let requiredBuildBytes: Int64 = ([\d_]+)", src)
    assert m, "requiredBuildBytes not found"
    assert int(m.group(1).replace("_", "")) == builder_cli.REQUIRED_BYTES

    m = re.search(r"public let inputMessageTemplates: \[String: String\] = \[(.*?)\]", src, re.S)
    assert m, "inputMessageTemplates not found"
    swift = dict(re.findall(r'"(\w+)":\s*"((?:[^"\\]|\\.)*)"', m.group(1)))
    assert swift == builder_cli.MESSAGES

    assert re.search(r'public let eventPrefix = "' + re.escape(builder_cli.EVENT) + '"', src)
    assert re.search(r'public static let searchPath = "' + re.escape(builder_cli.FIXED_PATH) + '"', src)


def test_swift_identifiers_agree():
    """The game's bundle id and the version key the Builder reads come from bw2/app.py."""
    from bw2 import app
    src = _swift_sources()
    assert re.search(r'public let gameBundleID = "' + re.escape(app.BUNDLE_ID) + '"', src)
    assert re.search(r'public let gameAppName = "' + re.escape(app.APP_NAME) + '"', src)
    plist = app.info_plist(app_version="9.9.9", builder_version="8.8.8")
    key = re.search(r'public let builderVersionKey = "(\w+)"', src)
    assert key and plist.get(key.group(1)) == "8.8.8"


def test_no_foreign_names():
    """Copied code keeps none of the NFSU2 Builder's names."""
    hits = []
    for p in sorted(PACKAGE.rglob("*")):
        if ".build" in p.relative_to(PACKAGE).parts or not p.is_file():
            continue
        text = p.read_text(errors="replace")
        if re.search(r"nfsu2|speed2|oyun|talha", text, re.I) or re.search(r"nfsu2|speed2", p.name, re.I):
            hits.append(str(p.relative_to(REPO)))
    assert hits == []
