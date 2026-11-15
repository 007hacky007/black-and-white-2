import hashlib
import json
import os
import subprocess
import sys
import threading
import time
import zipfile
import zlib
from pathlib import Path

import pytest

from bw2 import builder_cli, disc, paths
from bw2.builder_cli import (MESSAGES, STEPS, WEIGHTS, InputError, Inputs, classify, clean, emit,
                             link_inputs)
from bw2.releases import load_releases
from bw2.workspace import SOURCES, copy_sources

REPO = paths.REPO
FIXED_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
QUIT_LINE = "[bw2] quit after 300 input reads (BW2_QUIT_AFTER_READS)"
DISC = "Black & White 2 (USA) (Disc {})"
WHEELS = {  # "Measured" item 5
    "capstone": ["fc269c5082e17d7f9f265cd6980e2fdc6ab572df913951d76db0357e06b8001d",
                 "58a874d9a6cb15122135b1385a782e5b54c8a4d9396161b6498465046d2e0442"],
    "pefile": ["76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f"],
    "unicorn": ["d6c93e0f60328d8f4a1792af3f834137a28050fcc2305f2ec01efe8558a9844e"],
}


@pytest.fixture
def cli_env(monkeypatch):
    """The build command sets PATH and BW2_FFMPEG_PREFIX for its process; put them back after the test."""
    monkeypatch.setenv("PATH", os.environ.get("PATH", ""))
    monkeypatch.setenv("BW2_FFMPEG_PREFIX", "unset")
    monkeypatch.delenv("BW2_FFMPEG_PREFIX")


def _events(text: str) -> list[dict]:
    return [json.loads(ln[len("BW2-EVENT "):]) for ln in text.splitlines() if ln.startswith("BW2-EVENT ")]


def test_event_lines(capsys):
    emit(step="lift", title="x")
    assert capsys.readouterr().out == 'BW2-EVENT {"step": "lift", "title": "x"}\n'


def test_steps_and_weights():
    assert [s for s, _ in STEPS] == ["tools", "discs", "install", "patches", "lift", "compile", "package"]
    assert STEPS[3] == ("patches", "Applying patches 1.1 and 1.2")
    assert list(WEIGHTS) == [s for s, _ in STEPS]
    assert all(isinstance(w, int) and w > 0 for w in WEIGHTS.values())
    assert sum(WEIGHTS.values()) == 100
    assert isinstance(builder_cli.REQUIRED_BYTES, int) and builder_cli.REQUIRED_BYTES > 10 * 2**30


def test_required_bytes_is_the_measured_drop():
    """The need is the largest fall of the volume's free space in a build from the zips (Task 6: 10,604,712 KB),
    plus 20%, not the workspace's du peak: du counts the APFS clones (install to game, game to app) in full."""
    drop = 10_604_712 * 1024
    assert drop * 1.2 <= builder_cli.REQUIRED_BYTES < drop * 1.2 + 100_000_000


# ---- classify ------------------------------------------------------------------------------------------------------

class FakeSet:
    """Fake discs and patches with a release table and a zip table that know them."""

    def __init__(self, root: Path):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self.rows, self.zip_rows = [], []
        # disc 1: a zip; disc 2: a cue and its bin; disc 3: a lone bin; disc 4: an iso
        b1 = b"disc 1 " * 100
        self.zip1 = self.zip(1, b1, "d1.zip")
        b2 = b"disc 2 " * 100
        self.bin2 = self.write("d2.bin", b2)
        self.cue2 = self.write("d2.cue", b'FILE "d2.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')
        self.known("disc", DISC.format(2), b2)
        self.bin3 = self.write("d3.bin", b"disc 3 " * 100)
        self.known("disc", DISC.format(3), self.bin3.read_bytes())
        self.iso4 = self.write("d4.iso", b"disc 4 iso " * 100)
        self.known("disc", DISC.format(4), self.iso4.read_bytes())
        self.p11 = self.write("BW2Patch1_1.exe", b"patch 1.1 " * 50)
        self.known("patch-1.1", "BW2Patch1_1.exe", self.p11.read_bytes())
        self.p12 = self.write("BW2Patch1_2.exe", b"patch 1.2 " * 50)
        self.known("patch-1.2", "BW2Patch1_2.exe", self.p12.read_bytes())
        self.fan = self.write("fan.exe", b"fan 1.42 " * 50)
        self.known("fanpatch-installer", "B&W2 Fan Patch v1.42 Installer.exe", self.fan.read_bytes())

    def write(self, name: str, data: bytes) -> Path:
        p = self.root / name
        p.write_bytes(data)
        return p

    def known(self, kind, name, data):
        self.rows.append((kind, name, len(data), hashlib.sha256(data).hexdigest()))

    def zip(self, n: int, data: bytes, name: str, crc=None) -> Path:
        z = self.root / name
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(DISC.format(n) + ".bin", data)
            zf.writestr(DISC.format(n) + ".cue", f'FILE "{DISC.format(n)}.bin" BINARY\n')
        self.zip_rows.append((n, DISC.format(n) + ".bin", len(data), zlib.crc32(data) if crc is None else crc))
        return z

    def tables(self, tmp: Path):
        tsv = tmp / "rel.tsv"
        tsv.write_text("".join(f"{k}\t{n}\t{s}\t{h}\t\n" for k, n, s, h in self.rows))
        zt = tmp / "zips.tsv"
        zt.write_text("# disc\tbin\tsize\tcrc32\n" + "".join(f"{n}\t{b}\t{s}\t{c:08x}\n" for n, b, s, c in self.zip_rows))
        return load_releases(tsv), builder_cli.load_zip_table(zt)

    def all(self):
        return [self.zip1, self.cue2, self.bin3, self.iso4, self.p11, self.p12, self.fan]


def test_classify_inputs(tmp_path):
    fs = FakeSet(tmp_path / "in")
    rel, zt = fs.tables(tmp_path)
    want = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    assert classify(fs.all(), releases=rel, zip_table=zt) == want
    # a folder stands for the files directly in it; the cue's bin is not a disc of its own
    (fs.root / ".DS_Store").write_bytes(b"x")
    (fs.root / "notes.txt").write_text("x")
    assert classify([fs.root], releases=rel, zip_table=zt) == want
    # the same file twice (a folder and the file in it) is one input
    assert classify([fs.root, fs.p11, fs.bin2], releases=rel, zip_table=zt) == want

    bad = FakeSet(tmp_path / "bad")
    wrong_disc = bad.write("other.bin", b"another release's disc 2 " * 10)     # no row knows it
    bad_crc = bad.zip(3, b"disc 3 damaged" * 10, "d3.zip", crc=0x12345678)
    lone_cue = bad.write("d4.cue", b'FILE "gone.bin" BINARY\n  TRACK 01 MODE1/2352\n')
    p11_copy = bad.write("copy of BW2Patch1_1.exe", bad.p11.read_bytes())
    stranger = bad.write("setup.exe", b"some other installer")
    rel, zt = bad.tables(tmp_path)
    with pytest.raises(InputError) as ei:
        classify([bad.zip1, bad.cue2, wrong_disc, bad_crc, lone_cue, bad.p11, p11_copy, bad.fan, stranger],
                 releases=rel, zip_table=zt)
    assert sorted(ei.value.problems) == sorted([
        "missing: disc 3", "missing: disc 4", "duplicate: patch 1.1", "missing: patch 1.2",
        "not a known file: other.bin", "not a known file: d3.zip",
        "d4.cue: the cue names gone.bin, which is not there", "not a known file: setup.exe"])
    assert str(ei.value) == "; ".join(ei.value.problems)
    # two different files that are the same disc
    rel, zt = fs.tables(tmp_path)
    dup = fs.write("again.bin", fs.bin3.read_bytes())
    with pytest.raises(InputError) as ei:
        classify([*fs.all(), dup], releases=rel, zip_table=zt)
    assert ei.value.problems == ["duplicate: disc 3"]
    missing = tmp_path / "nowhere.zip"
    with pytest.raises(InputError) as ei:
        classify([*fs.all(), missing], releases=rel, zip_table=zt)
    assert ei.value.problems == [MESSAGES["not_found"].format(name=str(missing))]


def test_classify_truncated_zip(tmp_path):
    fs = FakeSet(tmp_path / "in")
    rel, zt = fs.tables(tmp_path)
    data = fs.zip1.read_bytes()
    fs.zip1.write_bytes(data[:len(data) // 2])
    with pytest.raises(InputError) as ei:
        classify(fs.all(), releases=rel, zip_table=zt)
    assert sorted(ei.value.problems) == ["missing: disc 1", "not a known file: d1.zip"]


def test_messages_are_templates():
    assert MESSAGES["missing"].format(what="disc 3") == "missing: disc 3"
    assert MESSAGES["unknown"].format(name="x") == "not a known file: x"
    assert MESSAGES["duplicate"].format(what="disc 2") == "duplicate: disc 2"
    assert MESSAGES["cue_bin"].format(name="a.cue", bin="a.bin") == "a.cue: the cue names a.bin, which is not there"


def _ws(tmp_path) -> Path:
    ws = tmp_path / "ws"
    copy_sources(ws)
    return ws


def _cli(ws: Path, *args: str, timeout=120) -> subprocess.CompletedProcess:
    env = {"PATH": FIXED_PATH, "HOME": os.environ["HOME"], "PYTHONUNBUFFERED": "1"}
    return subprocess.run([sys.executable, "-m", "bw2.builder_cli", *args], cwd=ws, env=env,
                          capture_output=True, text=True, timeout=timeout)


def test_build_refuses_before_writing(tmp_path):
    ws = _ws(tmp_path)
    fs = FakeSet(tmp_path / "in")
    r = _cli(ws, "build", "--input", str(fs.p11), str(fs.bin3), "--builder-version", "0.1.0")
    assert r.returncode == 1, r.stderr
    errors = [e["error"] for e in _events(r.stdout) if "error" in e]
    assert len(errors) == 1 and "missing: disc 3" in errors[0].split("; "), r.stdout
    assert "not a known file: d3.bin" in errors[0].split("; ")
    assert not [e for e in _events(r.stdout) if "step" in e]
    assert not (ws / "gamefiles").exists() and not (ws / "gen").exists() and not (ws / "out").exists()


def test_space_check_names_volume_and_numbers(tmp_path, monkeypatch):
    from bw2.diskspace import NotEnoughSpace
    monkeypatch.setattr(builder_cli.shutil, "disk_usage", lambda p: type("U", (), {"free": 1234})())
    with pytest.raises(NotEnoughSpace) as ei:
        builder_cli.check_space(tmp_path, 5678)
    msg = str(ei.value)
    assert msg == MESSAGES["space"].format(volume=builder_cli.volume_of(tmp_path), need=5678, free=1234)
    assert "5678" in msg and "1234" in msg and builder_cli.volume_of(tmp_path) in msg
    assert builder_cli.volume_of(tmp_path) == "/" or os.path.ismount(builder_cli.volume_of(tmp_path))


def test_space_check_test_override(tmp_path, monkeypatch):
    """BW2_REQUIRED_BYTES (a test switch for the end-to-end builds) replaces REQUIRED_BYTES; an explicit need wins."""
    from bw2.diskspace import NotEnoughSpace
    monkeypatch.setattr(builder_cli.shutil, "disk_usage", lambda p: type("U", (), {"free": 1000})())
    monkeypatch.setattr("bw2.diskspace.shutil.disk_usage", lambda p: type("U", (), {"free": 1000})())
    with pytest.raises(NotEnoughSpace):
        builder_cli.check_space(tmp_path)
    monkeypatch.setenv("BW2_REQUIRED_BYTES", "999")
    builder_cli.check_space(tmp_path)
    with pytest.raises(NotEnoughSpace, match="1001"):
        builder_cli.check_space(tmp_path, 1001)
    monkeypatch.setenv("BW2_REQUIRED_BYTES", "lots")
    with pytest.raises(ValueError, match="BW2_REQUIRED_BYTES"):
        builder_cli.check_space(tmp_path)


def test_build_refuses_without_space(tmp_path, monkeypatch, capsys, cli_env):
    fs = FakeSet(tmp_path / "in")
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    monkeypatch.setattr(builder_cli, "classify", lambda paths: inputs)
    monkeypatch.setattr(builder_cli, "ROOT", tmp_path / "ws")
    monkeypatch.setattr(builder_cli, "REQUIRED_BYTES", 1 << 60)
    monkeypatch.setattr(builder_cli, "STEP_FUNCS", {})
    assert builder_cli.main(["build", "--input", "x", "--builder-version", "0.1.0"]) == 1
    ev = _events(capsys.readouterr().out)
    assert len(ev) == 1 and ev[0]["error"].startswith("not enough free space on /"), ev
    assert str(1 << 60) in ev[0]["error"]
    assert not (tmp_path / "ws").exists()


def test_build_again_removes_an_earlier_builds_files_first(tmp_path, monkeypatch, capsys, cli_env):
    """A cancelled or failed build leaves gigabytes in the workspace; the next build removes them (as clean does,
    out/tools kept) before its space check, so they never count against it. A development tree is not touched."""
    from bw2.diskspace import NotEnoughSpace
    fs = FakeSet(tmp_path / "in")
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    monkeypatch.setattr(builder_cli, "classify", lambda paths: inputs)
    monkeypatch.setattr(builder_cli, "STEP_FUNCS", {})
    seen = []

    def check(root, need=None):
        seen.append(sorted(str(p.relative_to(root)) for p in Path(root).rglob("*") if p.is_file()))
        raise NotEnoughSpace("stop here")

    monkeypatch.setattr(builder_cli, "check_space", check)
    left = ["gamefiles/install/data.bin", "gamefiles/discs/d1/d1.bin", "gen/white.c", "out/bw2",
            "out/Black & White 2.app.part/Contents/x", "out/tools/unshield/unshield"]
    for root, sources in ((tmp_path / "ws", True), (tmp_path / "dev", False)):
        for rel in left:
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_bytes(b"left")
        if sources:
            (root / "SOURCES.txt").write_text("bw2\n")
        monkeypatch.setattr(builder_cli, "ROOT", root)
        assert builder_cli.main(["build", "--input", "x", "--builder-version", "0.1.0", "--app-version", "9"]) == 1
        assert _events(capsys.readouterr().out) == [{"error": "stop here"}]
    assert seen[0] == ["SOURCES.txt", "out/tools/unshield/unshield"]
    assert seen[1] == sorted(left)


# ---- linking and cleaning ------------------------------------------------------------------------------------------

def test_link_inputs(tmp_path):
    fs = FakeSet(tmp_path / "in")
    ws = tmp_path / "ws"
    g = ws / "gamefiles"
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    stale = g / "patches" / "BW2Patch1_2.exe"
    stale.parent.mkdir(parents=True)
    stale.symlink_to(tmp_path / "gone.exe")              # a stale link from an earlier build
    names = {"patch-1.1": "BW2Patch1_1.exe", "patch-1.2": "BW2Patch1_2.exe",
             "fanpatch-installer": "B&W2 Fan Patch v1.42 Installer.exe"}
    link_inputs(inputs, g, patch_names=names)
    cue = g / "discs" / "d2" / "d2.cue"
    bin2 = g / "discs" / "d2" / "d2.bin"
    linked = [cue, bin2, g / "discs" / "d3" / "d3.bin", g / "discs" / "d4" / "d4.iso",
              g / "patches" / "BW2Patch1_1.exe", g / "patches" / "BW2Patch1_2.exe",
              g / "patches" / "B&W2 Fan Patch v1.42 Installer.exe"]
    for p in linked:
        assert p.is_symlink() and os.path.isabs(os.readlink(p)), p
    assert Path(os.readlink(bin2)) == fs.bin2 and Path(os.readlink(g / "patches" / "BW2Patch1_2.exe")) == fs.p12
    assert not (g / "discs" / "d1").exists()             # zips are unpacked by identify_discs, not linked
    for src in fs.all() + [fs.bin2]:
        assert os.stat(src).st_nlink == 1, src           # never a hard link
    assert disc._parse_cue(cue) == bin2
    link_inputs(inputs, g, patch_names=names)            # again: the links are replaced, not refused
    assert cue.is_symlink()
    (ws / "gen" / "obj").mkdir(parents=True)
    (ws / "out" / "tools" / "ffmpeg-9.0.2" / "lib").mkdir(parents=True)
    (ws / "out" / "tools" / "unshield").write_bytes(b"x")
    (ws / "out" / "bw2").write_bytes(b"x")
    (ws / "out" / "Black & White 2.app" / "Contents").mkdir(parents=True)
    (ws / "out" / "elsewhere").symlink_to(fs.root)       # a link out of the workspace: removed, never followed
    (ws / SOURCES).write_text("VERSION\n")                # what makes ws a Builder workspace
    clean(ws)
    assert not (ws / "gamefiles").exists() and not (ws / "gen").exists()
    assert sorted(p.name for p in (ws / "out").iterdir()) == ["tools"]
    assert (ws / "out" / "tools" / "unshield").is_file() and (ws / "out" / "tools" / "ffmpeg-9.0.2" / "lib").is_dir()
    for src in fs.all() + [fs.bin2]:
        assert src.is_file(), src


def test_link_inputs_refuses_a_file_in_the_way(tmp_path):
    fs = FakeSet(tmp_path / "in")
    g = tmp_path / "ws" / "gamefiles"
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    names = {"patch-1.1": "BW2Patch1_1.exe", "patch-1.2": "BW2Patch1_2.exe", "fanpatch-installer": "fan.exe"}
    same = g / "patches" / "BW2Patch1_1.exe"
    same.parent.mkdir(parents=True)
    same.write_bytes(fs.p11.read_bytes())                # the same content: kept as it is
    other = g / "patches" / "BW2Patch1_2.exe"
    other.write_bytes(b"something else")
    with pytest.raises(builder_cli.LinkError) as ei:
        link_inputs(inputs, g, patch_names=names)
    assert str(other) in str(ei.value)
    assert other.read_bytes() == b"something else" and same.read_bytes() == fs.p11.read_bytes()


def test_clean_refuses_a_development_tree():
    assert not (REPO / SOURCES).exists()
    with pytest.raises(builder_cli.CleanError):
        clean(REPO)


# ---- the workspace -------------------------------------------------------------------------------------------------

def _git_files() -> set[str]:
    r = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z"], capture_output=True, check=True)
    return set(r.stdout.decode().split("\0")) - {""}


def test_copy_sources(tmp_path):
    ws = tmp_path / "ws"
    got = copy_sources(ws)
    tracked = _git_files()
    assert got and set(got) <= tracked
    assert got == sorted(got)
    assert not [p for p in got if p.startswith(("gamefiles/", "gen/", "out/", "tests/", "docs/"))]
    for want in ("bw2/catalog/white-1.42.functions.json", "bw2/manifests/game-usa-1.2-fan1.42.tsv",
                 "third_party/unshield/LICENSE", "runtime/bw2/main.c", "requirements-builder.txt", "VERSION",
                 "bw2/manifests/disc-zips.tsv", "tools/unshield/openssl/evp.h"):
        assert want in got, want
    for rel in got:
        assert (ws / rel).read_bytes() == (REPO / rel).read_bytes(), rel
    assert (ws / SOURCES).read_text().splitlines() == got
    again = tmp_path / "again"
    assert copy_sources(again, root=ws) == got
    # a later copy over an older one removes what the new list no longer has
    (ws / "bw2" / "old.py").write_text("x")
    (ws / SOURCES).write_text("\n".join(got + ["bw2/old.py"]) + "\n")
    assert copy_sources(again, root=ws) == sorted(got + ["bw2/old.py"])
    (ws / "bw2" / "old.py").unlink()
    (ws / SOURCES).write_text("\n".join(got) + "\n")
    copy_sources(again, root=ws)
    assert not (again / "bw2" / "old.py").exists()


def test_copy_sources_needs_git_or_manifest(tmp_path):
    from bw2.workspace import SourcesError
    (tmp_path / "src").mkdir()
    with pytest.raises(SourcesError):
        copy_sources(tmp_path / "dest", root=tmp_path / "src")
    (tmp_path / "src" / SOURCES).write_text("../outside.txt\n")
    with pytest.raises(SourcesError):
        copy_sources(tmp_path / "dest", root=tmp_path / "src")


def _requirements() -> dict[str, tuple[str, list[str]]]:
    text = (REPO / "requirements-builder.txt").read_text().replace("\\\n", " ")
    out = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip() if not line.lstrip().startswith("--hash") else line.strip()
        if not line:
            continue
        spec, *opts = line.split()
        assert "==" in spec, line
        hashes = [o[len("--hash=sha256:"):] for o in opts if o.startswith("--hash=sha256:")]
        assert hashes and len(hashes) == len(opts), line
        name, ver = spec.split("==")
        out[name] = (ver, hashes)
    return out


def test_requirements_pinned():
    req = _requirements()
    assert {n: v for n, (v, _) in req.items()} == {"capstone": "5.0.9", "pefile": "2024.8.26", "unicorn": "2.1.4"}
    assert {n: sorted(h) for n, (_, h) in req.items()} == {n: sorted(h) for n, h in WHEELS.items()}


@pytest.mark.gamefiles
def test_setup_venv(tmp_path):
    venv = tmp_path / "venv"
    r = _cli(REPO, "setup", "--venv", str(venv), timeout=600)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    assert {"done": str(venv)} in _events(r.stdout)
    py = venv / "bin" / "python"
    r2 = subprocess.run([str(py), "-c", "import capstone, pefile, unicorn; print('ok')"],
                        capture_output=True, text=True, env={"PATH": FIXED_PATH})
    assert r2.stdout == "ok\n", r2.stderr
    want = hashlib.sha256((REPO / "requirements-builder.txt").read_bytes()).hexdigest()
    assert (venv / ".build-requirements-sha256").read_text().strip() == want


# The Builder runs setup with its bare private Python, before the build's packages exist (found by plan 13
# Task 8's window end to end). This prelude makes the three packages unimportable, as in that Python.
BARE = ("import sys, subprocess\n"
        "for n in ('pefile', 'capstone', 'unicorn'): sys.modules[n] = None\n"
        "from bw2 import builder_cli\n")


def _bare(code: str, tmp_path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-c", BARE + code], cwd=REPO, capture_output=True, text=True,
                          env={"PATH": FIXED_PATH, "HOME": str(tmp_path)}, timeout=120)


def test_setup_without_the_packages(tmp_path):
    venv = tmp_path / "venv"
    code = ("calls = []\n"
            "def run(argv, **kw):\n"
            "    calls.append(argv)\n"
            "    return subprocess.CompletedProcess(argv, 0)\n"
            "builder_cli.subprocess.run = run\n"
            f"rc = builder_cli.main(['setup', '--venv', {str(venv)!r}])\n"
            "print('calls', len(calls))\n"
            "sys.exit(rc)\n")
    venv.mkdir()
    r = _bare(code, tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "calls 2" in r.stdout
    assert {"done": str(venv)} in _events(r.stdout)
    assert (venv / ".build-requirements-sha256").is_file()


def test_build_without_the_packages_names_them(tmp_path):
    r = _bare("sys.exit(builder_cli.main(['build', '--input', 'x', '--builder-version', '1']))", tmp_path)
    assert r.returncode == 1, r.stdout + r.stderr
    errors = [e["error"] for e in _events(r.stdout) if "error" in e]
    assert len(errors) == 1 and "pefile" in errors[0] and "setup" in errors[0], r.stdout + r.stderr


def test_clean_and_download_without_the_packages(tmp_path):
    r = _bare("sys.exit(builder_cli.main(['clean']))", tmp_path)
    assert r.returncode == 1                       # the repo is not a workspace: refused by name, not by import
    assert "only a Builder workspace is cleaned" in r.stdout, r.stdout + r.stderr


# ---- the steps -----------------------------------------------------------------------------------------------------

def test_build_again_after_interrupt(tmp_path, monkeypatch, cli_env):
    """Each step's start removes the .part outputs an interrupted run left, before any work starts."""
    ws = tmp_path / "ws"
    fs = FakeSet(tmp_path / "in")
    g, out = ws / "gamefiles", ws / "out"
    parts = {"tools": [out / "tools" / "ffmpeg-9.0.2.part", out / "tools" / "ffmpeg-9.0.2.src.part",
                       out / "tools" / "unshield.part"],
             "discs": [g / "discs" / "d1" / "d1.bin.part"],
             "install": [g / "install.part"],
             "patches": [g / "game.part"],
             "compile": [out / "bw2.part"],
             "package": [out / "Black & White 2.app.part"]}
    files = {"d1.bin.part", "unshield.part", "bw2.part"}
    for ps in parts.values():
        for p in ps:
            if p.name in files:
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b"half")
            else:
                (p / "half").mkdir(parents=True)
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    rel, zt = fs.tables(tmp_path)
    assert classify(fs.all(), releases=rel, zip_table=zt) == inputs     # classify reads only the inputs
    assert all(os.path.lexists(p) for ps in parts.values() for p in ps)
    seen = []

    def at_start(step):
        def fake(*a, **k):
            gone = [p for p in parts[step] if os.path.lexists(p)]
            assert not gone, (step, gone)
            seen.append(step)
            return fake_results.get(step, lambda: None)()
        return fake

    def game():
        (g / "game").mkdir(parents=True, exist_ok=True)
        (g / "game" / "white.exe").write_bytes(b"MZ fake")
        return g / "game"

    fake_results = {"patches": game, "package": lambda: out / "Black & White 2.app",
                    "discs": lambda: {1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}}
    monkeypatch.setattr(builder_cli.tools, "unshield", lambda *a, **k: None)
    monkeypatch.setattr(builder_cli.ffmpeg, "build_ffmpeg", at_start("tools"))
    monkeypatch.setattr(builder_cli.disc, "identify_discs", at_start("discs"))
    monkeypatch.setattr(builder_cli.install, "extract_install", at_start("install"))
    monkeypatch.setattr(builder_cli.gametree, "build_game_tree", at_start("patches"))
    monkeypatch.setattr(builder_cli, "d3dx_dll", lambda *a, **k: None)
    monkeypatch.setattr(builder_cli, "_check_white_exe", lambda p: None)
    monkeypatch.setattr(builder_cli.pipeline, "run_pipeline",
                        lambda **k: at_start("compile" if k.get("do_build") else "lift")())
    parts["lift"] = []
    monkeypatch.setattr(builder_cli.app, "package_app", at_start("package"))
    names = {"patch-1.1": "BW2Patch1_1.exe", "patch-1.2": "BW2Patch1_2.exe",
             "fanpatch-installer": "B&W2 Fan Patch v1.42 Installer.exe"}
    monkeypatch.setattr(builder_cli, "patch_names", lambda: names)
    ctx = builder_cli.Context(root=ws, inputs=inputs, app_version="0.1.0", builder_version="0.1.0")
    for step, _ in STEPS:
        builder_cli.STEP_FUNCS[step](ctx)
    assert seen == [s for s, _ in STEPS]
    assert ctx.app == out / "Black & White 2.app"
    assert (g / "exe" / "white.exe").read_bytes() == b"MZ fake"


def test_build_runs_steps_in_order(tmp_path, monkeypatch, capsys, cli_env):
    """The build command: step events in order, the step's title on a failure, exit 1."""
    fs = FakeSet(tmp_path / "in")
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    monkeypatch.setattr(builder_cli, "classify", lambda paths: inputs)
    monkeypatch.setattr(builder_cli, "check_space", lambda root, need=0: None)
    monkeypatch.setattr(builder_cli, "ROOT", tmp_path / "ws")
    ran = []

    def fake(step):
        def run(ctx):
            ran.append(step)
            if step == "package":
                ctx.app = tmp_path / "ws" / "out" / "Black & White 2.app"
        return run

    monkeypatch.setattr(builder_cli, "STEP_FUNCS", {s: fake(s) for s, _ in STEPS})
    assert builder_cli.main(["build", "--input", "x", "--builder-version", "0.1.0", "--app-version", "9"]) == 0
    ev = _events(capsys.readouterr().out)
    assert [e["step"] for e in ev if "step" in e] == [s for s, _ in STEPS]
    assert [e["title"] for e in ev if "step" in e] == [t for _, t in STEPS]
    assert ev[-1] == {"done": str(tmp_path / "ws" / "out" / "Black & White 2.app")}
    assert all(0 <= e["progress"] <= 1 for e in ev if "progress" in e)
    assert ran == [s for s, _ in STEPS]

    def boom(ctx):
        raise RuntimeError("unshield failed: bad cabinet")

    monkeypatch.setitem(builder_cli.STEP_FUNCS, "install", boom)
    assert builder_cli.main(["build", "--input", "x", "--builder-version", "0.1.0"]) == 1
    ev = _events(capsys.readouterr().out)
    assert ev[-1] == {"error": "Unpacking the game: unshield failed: bad cabinet"}
    assert [e["step"] for e in ev if "step" in e] == ["tools", "discs", "install"]


def test_build_sets_fixed_path_and_ffmpeg_prefix(tmp_path, monkeypatch, capsys, cli_env):
    fs = FakeSet(tmp_path / "in")
    inputs = Inputs(discs={1: fs.zip1, 2: fs.cue2, 3: fs.bin3, 4: fs.iso4}, patch11=fs.p11, patch12=fs.p12, fan=fs.fan)
    monkeypatch.setattr(builder_cli, "classify", lambda paths: inputs)
    monkeypatch.setattr(builder_cli, "check_space", lambda root, need=0: None)
    os.environ["PATH"] = "/opt/homebrew/bin:/usr/bin:/bin"
    seen = {}

    def tools_step(ctx):
        r = subprocess.run(["/bin/sh", "-c", 'echo "$PATH"'], capture_output=True, text=True)
        seen["path"] = r.stdout.strip()
        seen["prefix"] = os.environ.get("BW2_FFMPEG_PREFIX")
        raise RuntimeError("stop here")

    monkeypatch.setitem(builder_cli.STEP_FUNCS, "tools", tools_step)
    assert builder_cli.main(["build", "--input", "x", "--builder-version", "0.1.0"]) == 1
    assert seen == {"path": FIXED_PATH, "prefix": str(builder_cli.ffmpeg.LGPL_PREFIX)}


# ---- downloads through the CLI -------------------------------------------------------------------------------------

def test_download_kinds():
    from bw2.download import PATCH_SOURCES
    assert PATCH_SOURCES == {
        "patch-1.1": ("https://community.pcgamingwiki.com/files/file/2151-black-white-2-v11-patch/", 10345),
        "patch-1.2": ("https://community.pcgamingwiki.com/files/file/2152-black-white-2-v12-patch/", 10346)}
    assert set(builder_cli.DOWNLOAD_KINDS) == {"patch-1.1", "patch-1.2", "fanpatch-installer"}


def test_download_command(tmp_path, monkeypatch, capsys, cli_env):
    calls = []

    def fake_pcgw(page, r, dest, expected, progress=None):
        calls.append((page, r, dest, expected.kind))
        progress(0.5)
        progress(1.0)
        return dest

    monkeypatch.setattr(builder_cli, "fetch_pcgw", fake_pcgw)
    assert builder_cli.main(["download", "--kind", "patch-1.2", "--dest", str(tmp_path)]) == 0
    ev = _events(capsys.readouterr().out)
    assert calls == [("https://community.pcgamingwiki.com/files/file/2152-black-white-2-v12-patch/", 10346,
                      tmp_path / "BW2Patch1_2.exe", "patch-1.2")]
    assert ev == [{"progress": 0.5}, {"progress": 1.0}, {"done": str(tmp_path / "BW2Patch1_2.exe")}]

    def fake_fetch(url, dest, expected, progress=None):
        raise builder_cli.FingerprintMismatch(f"{url}: download does not match {expected.name}")

    monkeypatch.setattr(builder_cli, "fetch", fake_fetch)
    assert builder_cli.main(["download", "--kind", "fanpatch-installer", "--dest", str(tmp_path)]) == 1
    ev = _events(capsys.readouterr().out)
    assert len(ev) == 1 and "does not match B&W2 Fan Patch v1.42 Installer.exe" in ev[0]["error"]


# ---- the whole build (opt-in) --------------------------------------------------------------------------------------

def _macho(p: Path) -> bool:
    with open(p, "rb") as f:
        return f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def _otool_lines(app: Path) -> dict[str, str]:
    from bw2.build import check_tools
    otool = check_tools().parent / "otool"
    out = {}
    for p in sorted(app.rglob("*")):
        if p.is_file() and not p.is_symlink() and _macho(p):
            r = subprocess.run([str(otool), "-L", str(p)], capture_output=True, text=True, check=True)
            out[str(p.relative_to(app))] = r.stdout
    return out


@pytest.mark.gamefiles
@pytest.mark.builder_e2e
def test_builder_e2e(tmp_path):
    from bw2.app import APP_NAME, install_app, verify_app
    ws = tmp_path / "ws"
    copy_sources(ws)
    (ws / ".venv").symlink_to(REPO / ".venv")
    zips = [paths.discs_dir() / f"{DISC.format(n)}.zip" for n in range(1, 5)]
    pdir = paths.GAMEFILES / "patches"
    patches = [pdir / "BW2Patch1_1.exe", pdir / "BW2Patch1_2.exe", pdir / "B&W2 Fan Patch v1.42 Installer.exe"]
    env = {"PATH": FIXED_PATH, "HOME": os.environ["HOME"], "DEVELOPER_DIR": "/Library/Developer/CommandLineTools",
           "PYTHONUNBUFFERED": "1"}
    t0 = time.monotonic()
    proc = subprocess.Popen([str(ws / ".venv" / "bin" / "python"), "-m", "bw2.builder_cli", "build", "--input",
                             *map(str, zips + patches), "--builder-version", "0.1.0"],
                            cwd=ws, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    timer = threading.Timer(3600, proc.kill)
    timer.start()
    events, tail = [], []
    try:
        for line in proc.stdout:
            t = time.monotonic() - t0
            print(f"[e2e +{t:7.1f} s] {line}", end="", flush=True)
            tail = (tail + [line])[-60:]
            if line.startswith("BW2-EVENT "):
                events.append((round(t, 1), json.loads(line[len("BW2-EVENT "):])))
        rc = proc.wait()
    finally:
        timer.cancel()
    print(f"[e2e] build exit {rc} after {time.monotonic() - t0:.1f} s; events: {events}", flush=True)
    assert rc == 0, "".join(tail)
    evs = [e for _, e in events]
    assert not [e for e in evs if "error" in e], evs
    assert [e["step"] for e in evs if "step" in e] == [s for s, _ in STEPS]
    built = ws / "out" / APP_NAME
    assert evs[-1] == {"done": str(built)}
    verify_app(built)
    links = _otool_lines(built)
    assert "Contents/MacOS/bw2" in links and len(links) >= 4, sorted(links)
    assert not {k: v for k, v in links.items() if "/opt/homebrew" in v}
    moved = install_app(built, tmp_path / "dest")
    r = subprocess.run([str(ws / ".venv" / "bin" / "python"), "-m", "bw2.builder_cli", "clean"],
                       cwd=ws, env=env, capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stdout + r.stderr
    assert not (ws / "gamefiles").exists() and not (ws / "gen").exists()
    assert sorted(p.name for p in (ws / "out").iterdir()) == ["tools"]
    verify_app(moved)
    support = tmp_path / "support"
    run_env = {"PATH": FIXED_PATH, "HOME": os.environ["HOME"], "BW2_HEADLESS": "1", "BW2_NO_ALERT": "1",
               "BW2_SUPPORT": str(support), "BW2_QUIT_AFTER_READS": "300"}
    r = subprocess.run([str(moved / "Contents" / "MacOS" / "bw2")], cwd="/", env=run_env, capture_output=True,
                       timeout=300)
    log = support / "Logs" / "bw2.log"
    lines = []
    for _ in range(50):                    # the watcher writes the log; give it a moment after the engine's exit
        lines = log.read_text(errors="replace").splitlines() if log.exists() else []
        if lines and lines[-1] == QUIT_LINE:
            break
        time.sleep(0.1)
    assert r.returncode == 0, lines[-30:]
    assert lines[-1] == QUIT_LINE, lines[-30:]
