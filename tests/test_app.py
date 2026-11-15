"""Packaging the app (plan 13 Task 5): bw2.app puts the engine, the lifted DLL, the game tree and the LGPL FFmpeg
libraries into `Black & White 2.app`, ad-hoc signs it, verifies it and only then moves it into place.

A fake engine is a small Mach-O compiled here with check_tools()'s clang and the SDK; the real game is used only by the
gamefiles tests."""
import os
import plistlib
import shutil
import subprocess
import time
import uuid
from pathlib import Path

import pytest

from bw2 import app as app_mod
from bw2 import paths
from bw2.app import (APP_NAME, BUNDLE_ID, ENGINE, PackageError, info_plist, install_app, package_app, sign_app,
                     verify_app, version)
from bw2.build import APP_RPATH, check_tools, install_name, sdk_path
from bw2.ffmpeg import FFMPEG_DYLIBS
from bw2.gametree import ManifestMismatch, verify_manifest
from bw2.install import write_manifest

HELLO = '#include <stdio.h>\nint main(void) { puts("ok"); return 0; }\n'
CALLS = ('#include <stdio.h>\nint bw2_fake_avformat(void); int bw2_fake_avcodec(void); int bw2_fake_avutil(void);\n'
         'int main(void) { if (bw2_fake_avformat() + bw2_fake_avcodec() + bw2_fake_avutil() != 3) return 1;\n'
         '  puts("ok"); return 0; }\n')
FFMPEG_LINK = ["-lavformat", "-lavcodec", "-lavutil"]


def _cc(out: Path, src: str, *args: str, dylib: bool = False) -> Path:
    out.parent.mkdir(parents=True, exist_ok=True)
    c = out.with_name(out.name + ".c")
    c.write_text(src)
    cmd = [str(check_tools()), "-isysroot", str(sdk_path()), "-mmacosx-version-min=13.0",
           *(["-dynamiclib"] if dylib else []), str(c), "-o", str(out), *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return out


def _engine(tmp: Path, src: str = HELLO, *args: str, name: str = "engine") -> Path:
    return _cc(tmp / name / "bw2", src, *args)


def _rpath_engine(tmp: Path, prefix: Path, *extra: str, name: str = "engine") -> Path:
    return _engine(tmp, CALLS, f"-L{prefix / 'lib'}", *FFMPEG_LINK, *extra, name=name)


def _game(tmp: Path) -> tuple[Path, Path]:
    game = tmp / "game src"
    files = {"white.exe": b"MZ" + bytes(100), "Data/a.txt": b"hello\n", "Data/Sub/b & c.bin": bytes(range(256))}
    for rel, data in files.items():
        (game / rel).parent.mkdir(parents=True, exist_ok=True)
        (game / rel).write_bytes(data)
    manifest = tmp / "game.tsv"
    write_manifest(game, manifest)
    return game, manifest


def _dll(tmp: Path) -> Path:
    p = tmp / "d3dx9_25.dll"
    p.write_bytes(b"MZ fake dll")
    return p


def _dest(tmp: Path) -> Path:
    d = tmp / "a & b"
    d.mkdir()
    return d


def _quarantine(p: Path) -> None:
    subprocess.run(["/usr/bin/xattr", "-w", "com.apple.quarantine", "0081;00000000;bw2-test;", str(p)], check=True)


def _xattrs(p: Path) -> str:
    return subprocess.run(["/usr/bin/xattr", "-lr", str(p)], capture_output=True, text=True, check=True).stdout


def _clean_env() -> dict:
    return {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "BW2_"))}


def test_info_plist():
    assert info_plist("1.2.3", "4.5.6") == {
        "CFBundleExecutable": "bw2",
        "CFBundleIdentifier": "local.bw2-port.game",
        "CFBundleName": "Black & White 2",
        "CFBundleDisplayName": "Black & White 2",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.2.3",
        "CFBundleVersion": "1.2.3",
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": "13.0",
        "LSArchitecturePriority": ["arm64"],
        "LSApplicationCategoryType": "public.app-category.strategy-games",
        "NSHighResolutionCapable": True,
        "BW2BuilderVersion": "4.5.6",
    }
    assert (APP_NAME, BUNDLE_ID, ENGINE) == ("Black & White 2.app", "local.bw2-port.game", "bw2")


def test_version():
    assert version() == "0.0.1"
    assert (paths.REPO / "VERSION").read_text() == "0.0.1\n"


def test_package_small(tmp_path):
    game, manifest = _game(tmp_path)
    _quarantine(game / "Data" / "a.txt")
    dll, dest = _dll(tmp_path), _dest(tmp_path)
    app = package_app(_engine(tmp_path), dll, None, game, dest, "1.2.3", "4.5.6", manifest=manifest)
    assert app == dest / APP_NAME and app.is_dir()
    verify_app(app)
    c = app / "Contents"
    assert os.listdir(c / "MacOS") == ["bw2"]
    assert os.access(c / "MacOS" / "bw2", os.X_OK)
    assert (c / "Resources" / "d3dx9_25.dll").read_bytes() == dll.read_bytes()
    verify_manifest(c / "Resources" / "game", manifest)
    assert not (c / "Frameworks").exists()
    assert plistlib.loads((c / "Info.plist").read_bytes()) == info_plist("1.2.3", "4.5.6")
    assert "com.apple.quarantine" not in _xattrs(app)
    assert "com.apple.quarantine" in _xattrs(game / "Data" / "a.txt")   # the source is left as it was
    assert os.listdir(dest) == [APP_NAME]
    r = subprocess.run([str(c / "MacOS" / "bw2")], capture_output=True, text=True, cwd="/", env=_clean_env())
    assert (r.returncode, r.stdout) == (0, "ok\n")


@pytest.mark.gamefiles
def test_package_refuses_homebrew_engine(tmp_path):
    engine = tmp_path / "engine" / "bw2"
    engine.parent.mkdir()
    shutil.copyfile(paths.REPO / "out" / "bw2", engine)
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    with pytest.raises(PackageError, match="/opt/homebrew"):
        package_app(engine, _dll(tmp_path), None, game, dest, "0.1.0", "0.1.0", manifest=manifest)
    assert os.listdir(dest) == []


def test_package_refuses_absolute_library(tmp_path, fake_ffmpeg_prefix):
    # beyond the plan: the same refusal without game files, from a fake prefix with Homebrew-style install names
    prefix = fake_ffmpeg_prefix(install_name=lambda n: f"/opt/homebrew/opt/ffmpeg/lib/{n}")
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    with pytest.raises(PackageError, match="/opt/homebrew/opt/ffmpeg/lib/libavformat.63.dylib"):
        package_app(_rpath_engine(tmp_path, prefix), _dll(tmp_path), None, game, dest, "0.1.0", "0.1.0",
                    manifest=manifest)
    assert os.listdir(dest) == []


def test_package_rpath_dylibs(tmp_path, fake_ffmpeg_prefix):
    prefix = fake_ffmpeg_prefix()
    engine = _rpath_engine(tmp_path, prefix, f"-Wl,-rpath,{APP_RPATH}")
    # unpackaged, dyld finds nothing through @executable_path/../Frameworks and stops the engine before main
    r = subprocess.run([str(engine)], capture_output=True, text=True, cwd="/", env=_clean_env())
    assert r.returncode != 0 and "ok" not in r.stdout, r
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    app = package_app(engine, _dll(tmp_path), prefix, game, dest, "0.1.0", "0.1.0", manifest=manifest)
    fw = app / "Contents" / "Frameworks"
    assert sorted(os.listdir(fw)) == sorted(FFMPEG_DYLIBS)
    for name in FFMPEG_DYLIBS:
        assert (fw / name).is_file() and not (fw / name).is_symlink(), name
        assert install_name(fw / name) == f"@rpath/{name}"
    verify_app(app)
    r = subprocess.run([str(app / "Contents" / "MacOS" / "bw2")], capture_output=True, text=True, cwd="/",
                       env=_clean_env())
    assert (r.returncode, r.stdout) == (0, "ok\n"), r.stderr


def test_package_refuses_foreign_rpath(tmp_path, fake_ffmpeg_prefix):
    prefix = fake_ffmpeg_prefix()
    foreign = tmp_path / "lib"
    engine = _rpath_engine(tmp_path, prefix, f"-Wl,-rpath,{APP_RPATH}", f"-Wl,-rpath,{foreign}")
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    with pytest.raises(PackageError, match=str(foreign)):
        package_app(engine, _dll(tmp_path), prefix, game, dest, "0.1.0", "0.1.0", manifest=manifest)
    assert os.listdir(dest) == []


@pytest.mark.parametrize("case", ["no-prefix", "no-rpath", "other-rpath-lib"])
def test_package_refuses_rpath_engine(tmp_path, fake_ffmpeg_prefix, case):
    # beyond the plan: an @rpath library the prefix does not give, and @rpath libraries with no LC_RPATH at all
    prefix = fake_ffmpeg_prefix()
    if case == "no-prefix":
        engine, given, want = _rpath_engine(tmp_path, prefix, f"-Wl,-rpath,{APP_RPATH}"), None, "@rpath/libav"
    elif case == "no-rpath":
        engine, given, want = _rpath_engine(tmp_path, prefix), prefix, "no LC_RPATH"
    else:
        other = _cc(tmp_path / "other" / "libother.dylib", "int bw2_other(void) { return 0; }\n",
                    "-install_name", "@rpath/libother.dylib", dylib=True)
        src = "int bw2_other(void);\nint main(void) { return bw2_other(); }\n"
        engine = _engine(tmp_path, src, f"-L{other.parent}", "-lother", f"-Wl,-rpath,{APP_RPATH}")
        given, want = prefix, "@rpath/libother.dylib"
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    with pytest.raises(PackageError, match=want):
        package_app(engine, _dll(tmp_path), given, game, dest, "0.1.0", "0.1.0", manifest=manifest)
    assert os.listdir(dest) == []


def test_package_refuses_homebrew_dylib(tmp_path, fake_ffmpeg_prefix):
    # beyond the plan: a library put into Frameworks may not link outside the system either
    prefix = fake_ffmpeg_prefix()
    dep = _cc(tmp_path / "dep" / "libdep.dylib", "int bw2_dep(void) { return 0; }\n",
              "-install_name", "/opt/homebrew/lib/libdep.dylib", dylib=True)
    real = (prefix / "lib" / FFMPEG_DYLIBS[0]).resolve()
    src = "int bw2_dep(void);\nint bw2_fake_ffmpeg(void) { return bw2_dep(); }\nint bw2_fake_avformat(void) { return 1; }\n"
    _cc(real, src, "-install_name", f"@rpath/{FFMPEG_DYLIBS[0]}", f"-L{dep.parent}", "-ldep", dylib=True)
    engine = _rpath_engine(tmp_path, prefix, f"-Wl,-rpath,{APP_RPATH}")
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    with pytest.raises(PackageError, match="/opt/homebrew/lib/libdep.dylib"):
        package_app(engine, _dll(tmp_path), prefix, game, dest, "0.1.0", "0.1.0", manifest=manifest)
    assert os.listdir(dest) == []


def test_package_refuses_changed_game(tmp_path):
    game, manifest = _game(tmp_path)
    p = game / "Data" / "a.txt"
    p.write_bytes(b"jello\n")
    dest = _dest(tmp_path)
    with pytest.raises(ManifestMismatch, match="changed Data/a.txt"):
        package_app(_engine(tmp_path), _dll(tmp_path), None, game, dest, "0.1.0", "0.1.0", manifest=manifest)
    assert os.listdir(dest) == []


def test_package_replaces_existing(tmp_path):
    game, manifest = _game(tmp_path)
    engine, dll, dest = _engine(tmp_path), _dll(tmp_path), _dest(tmp_path)
    old = package_app(engine, dll, None, game, dest, "0.0.1", "0.0.1", manifest=manifest)
    (old / "Contents" / "Resources" / "marker.txt").write_text("old app\n")
    stale = dest / (APP_NAME + ".part")
    (stale / "Contents").mkdir(parents=True)
    (stale / "Contents" / "junk").write_text("half-built\n")
    app = package_app(engine, dll, None, game, dest, "0.2.0", "0.2.0", manifest=manifest)
    assert app == old
    assert not (app / "Contents" / "Resources" / "marker.txt").exists()
    assert os.listdir(dest) == [APP_NAME]
    verify_app(app)
    assert plistlib.loads((app / "Contents" / "Info.plist").read_bytes())["CFBundleVersion"] == "0.2.0"


def _small_app(tmp: Path, where: str, ver: str) -> Path:
    game, manifest = _game(tmp / f"game-{where}")
    d = tmp / where
    d.mkdir()
    return package_app(_engine(tmp, name=f"engine-{where}"), _dll(tmp / f"game-{where}"), None, game, d, ver, ver,
                       manifest=manifest)


def test_install_app_restores_old_on_failed_rename(tmp_path, monkeypatch):
    # beyond the plan: when the new app cannot be renamed into place, the old one goes back
    old = _small_app(tmp_path, "dest", "0.0.1")
    (old / "Contents" / "Resources" / "marker.txt").write_text("old app\n")
    built = _small_app(tmp_path, "build", "0.2.0")
    real_rename = os.rename

    def rename(src, dst):
        if Path(dst) == old and Path(src).name == APP_NAME:
            raise OSError(5, "Input/output error", str(dst))
        return real_rename(src, dst)
    monkeypatch.setattr(app_mod.os, "rename", rename)
    with pytest.raises(PackageError, match="Input/output error"):
        install_app(built, old.parent)
    assert (old / "Contents" / "Resources" / "marker.txt").read_text() == "old app\n"
    assert os.listdir(old.parent) == [APP_NAME]
    assert built.is_dir()


def test_install_app_other_volume(tmp_path, monkeypatch):
    # An app on another volume is copied with its final bundle name, verified, then renamed.
    old = _small_app(tmp_path, "dest", "0.0.1")
    built = _small_app(tmp_path, "build", "0.2.0")
    monkeypatch.setattr(app_mod, "_same_volume", lambda a, b: False)
    calls = []
    real_run = subprocess.run

    def run(cmd, *a, **kw):
        calls.append(list(map(str, cmd)))
        return real_run(cmd, *a, **kw)
    monkeypatch.setattr(app_mod.subprocess, "run", run)
    app = install_app(built, old.parent)
    assert app == old
    copies = [c for c in calls if c[:3] == ["/bin/cp", "-c", "-R"]]
    assert len(copies) == 1 and copies[0][3] == str(built)
    staged = Path(copies[0][4])
    assert staged.name == APP_NAME
    assert staged.parent.parent == old.parent
    assert staged.parent.name.startswith(".bw2-install-")
    assert not staged.parent.exists()
    assert os.listdir(old.parent) == [APP_NAME]
    verify_app(app)
    assert plistlib.loads((app / "Contents" / "Info.plist").read_bytes())["CFBundleVersion"] == "0.2.0"


def test_sign_and_verify_fail_with_codesign_output(tmp_path):
    # beyond the plan: a damaged bundle fails with codesign's own words
    app = _small_app(tmp_path, "dest", "0.1.0")
    (app / "Contents" / "MacOS" / "bw2").write_bytes(b"\0" * 64)
    with pytest.raises(PackageError, match="codesign --verify --deep --strict .*\n.*not signed"):
        verify_app(app)
    (app / "Contents" / "Info.plist").unlink()
    with pytest.raises(PackageError, match="codesign --force --sign - .*\n.*bundle format unrecognized"):
        sign_app(app)


def test_cli_defaults(tmp_path, monkeypatch):
    got = {}

    def fake(engine, d3dx_dll, ffmpeg_prefix, game, dest_dir, app_version, builder_version):
        got.update(engine=engine, dll=d3dx_dll, prefix=ffmpeg_prefix, game=game, dest=dest_dir,
                   app_version=app_version, builder_version=builder_version)
        return dest_dir / APP_NAME
    monkeypatch.setattr(app_mod, "package_app", fake)
    assert app_mod.main(["--engine", "e/bw2", "--ffmpeg", "p", "--out", str(tmp_path)]) == 0
    assert got == dict(engine=Path("e/bw2"), dll=paths.REPO / "out" / "d3dx9_25.dll", prefix=Path("p"),
                       game=paths.GAMEFILES / "game", dest=tmp_path, app_version="0.0.1", builder_version="0.0.1")
    assert app_mod.main(["--engine", "e", "--ffmpeg", "p", "--out", str(tmp_path), "--app-version", "2.0",
                         "--builder-version", "3.0"]) == 0
    assert (got["app_version"], got["builder_version"]) == ("2.0", "3.0")


def test_cli_refusal_exits_1(tmp_path, monkeypatch, capsys):
    def fake(*a, **kw):
        raise PackageError("engine e links /opt/homebrew/x")
    monkeypatch.setattr(app_mod, "package_app", fake)
    assert app_mod.main(["--engine", "e", "--ffmpeg", "p", "--out", str(tmp_path)]) == 1
    assert "bw2.app: engine e links /opt/homebrew/x" in capsys.readouterr().err


@pytest.mark.gamefiles
def test_packaged_app_new_game(lgpl_engine, tmp_path):
    from test_boot import CAMPAIGN_QUIT, INTRO_LAST, NEW_GAME_SCRIPT, engine_lines
    from bw2.ffmpeg import LGPL_PREFIX
    dest = _dest(tmp_path)
    t0 = time.monotonic()
    app = package_app(lgpl_engine, lgpl_engine.parent / "d3dx9_25.dll", LGPL_PREFIX, paths.GAMEFILES / "game", dest,
                      version(), version())
    print(f"package_app: {time.monotonic() - t0:.1f} s")
    support = tmp_path / "support"
    script = tmp_path / "input.script"
    script.write_text(NEW_GAME_SCRIPT)
    env = _clean_env()
    env.update(BW2_HEADLESS="1", BW2_NO_ALERT="1", BW2_SUPPORT=str(support), BW2_INPUT_SCRIPT=str(script),
               BW2_QUIT_AFTER_MOVIE=f"1798:{INTRO_LAST}+600")
    r = subprocess.run([str(app / "Contents" / "MacOS" / "bw2")], capture_output=True, cwd="/", env=env, timeout=900)
    log = support / "Logs" / "bw2.log"
    lines = []
    for _ in range(50):               # the watcher writes the log; give it a moment after the engine's exit
        lines = engine_lines(log.read_text(errors="replace")) if log.exists() else []
        if lines and lines[-1] == CAMPAIGN_QUIT:
            break
        time.sleep(0.1)
    assert r.returncode == 0, lines[-30:]
    assert lines[-1] == CAMPAIGN_QUIT, lines[-30:]
    assert not [ln for ln in r.stdout.decode(errors="replace").splitlines() if ln.startswith("[bw2] alert: ")]
    assert "owned by a later plan" not in "\n".join(lines)
    verify_app(app)


def test_package_keeps_bundle_name_during_signing(tmp_path, monkeypatch):
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    real_sign = app_mod.sign_app
    signed = []

    def sign(bundle):
        signed.append(bundle)
        assert bundle.name == APP_NAME
        assert bundle.parent.name.startswith(".bw2-install-")
        return real_sign(bundle)

    monkeypatch.setattr(app_mod, "sign_app", sign)
    package_app(_engine(tmp_path), _dll(tmp_path), None, game, dest, "1", "1", manifest=manifest)
    assert len(signed) == 1
    assert list(dest.iterdir()) == [dest / APP_NAME]


def test_install_registers_and_refreshes_bundle(tmp_path, monkeypatch):
    built = _small_app(tmp_path, "build", "1")
    os.utime(built, (1, 1))
    calls = []
    real_run = subprocess.run

    def run(cmd, *args, **kwargs):
        if str(cmd[0]).endswith("/lsregister"):
            calls.append(cmd)
            assert Path(cmd[-1]).is_dir()
            return subprocess.CompletedProcess(cmd, 0)
        return real_run(cmd, *args, **kwargs)

    monkeypatch.setattr(app_mod.subprocess, "run", run)
    target = install_app(built, tmp_path / "dest")
    assert calls == [[app_mod.LSREGISTER, "-f", str(target)]]
    assert target.stat().st_mtime > time.time() - 10
    verify_app(target)


def test_install_sweeps_leftovers_without_following_symlinks(tmp_path):
    built = _small_app(tmp_path, "build", "1")
    dest = _dest(tmp_path)
    outside = tmp_path / "keep"
    outside.mkdir()
    (outside / "keep.txt").write_text("keep")
    (dest / f".bw2-install-{uuid.uuid4()}").mkdir()
    (dest / f".bw2-install-{uuid.uuid4()}").symlink_to(outside, target_is_directory=True)
    target = install_app(built, dest)
    assert list(dest.iterdir()) == [target]
    assert (outside / "keep.txt").read_text() == "keep"


def test_install_copy_failure_keeps_old_and_source(tmp_path, monkeypatch):
    old = _small_app(tmp_path, "dest", "1")
    built = _small_app(tmp_path, "build", "2")
    monkeypatch.setattr(app_mod, "_same_volume", lambda a, b: False)
    real_verify = app_mod.verify_app

    def verify(bundle):
        if bundle.parent.name.startswith(".bw2-install-"):
            raise PackageError("bad copy")
        real_verify(bundle)

    monkeypatch.setattr(app_mod, "verify_app", verify)
    with pytest.raises(PackageError, match="bad copy"):
        install_app(built, old.parent)
    assert built.is_dir()
    assert plistlib.loads((old / "Contents/Info.plist").read_bytes())["CFBundleVersion"] == "1"
    assert list(old.parent.iterdir()) == [old]


def test_package_adds_game_icon_before_signing(tmp_path, monkeypatch):
    from bw2 import appicon
    game, manifest = _game(tmp_path)
    dest = _dest(tmp_path)
    icon = b"icns" + (8).to_bytes(4, "big")

    def make_icon(exe, target):
        assert exe == game / "white.exe"
        target.write_bytes(icon)
        return True

    monkeypatch.setattr(appicon, "make_game_icon", make_icon)
    real_sign = app_mod.sign_app

    def sign(bundle):
        info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
        assert info["CFBundleIconFile"] == "AppIcon.icns"
        assert (bundle / "Contents/Resources/AppIcon.icns").read_bytes() == icon
        real_sign(bundle)

    monkeypatch.setattr(app_mod, "sign_app", sign)
    target = package_app(_engine(tmp_path), _dll(tmp_path), None, game, dest, "1", "1", manifest=manifest)
    verify_app(target)


def test_package_logs_unusable_game_icon(tmp_path, capsys):
    game, manifest = _game(tmp_path)
    package_app(_engine(tmp_path), _dll(tmp_path), None, game, _dest(tmp_path), "1", "1", manifest=manifest)
    assert "no usable icon" in capsys.readouterr().out.lower()


def test_install_recovers_known_backup_before_failed_retry(tmp_path, monkeypatch):
    old = _small_app(tmp_path, "dest", "1")
    built = _small_app(tmp_path, "build", "2")
    backup = old.parent / f".trash-{uuid.uuid4()}"
    old.rename(backup)
    unrelated = old.parent / ".trash-other-app"
    unrelated.mkdir()
    (unrelated / "keep").write_text("kept")

    def verify(bundle):
        raise PackageError("new build is damaged")

    monkeypatch.setattr(app_mod, "verify_app", verify)
    with pytest.raises(PackageError, match="new build is damaged"):
        install_app(built, old.parent)
    assert old.is_dir()
    assert plistlib.loads((old / "Contents/Info.plist").read_bytes())["CFBundleVersion"] == "1"
    assert built.is_dir()
    assert (unrelated / "keep").read_text() == "kept"


def test_install_sweep_keeps_backup_beside_unrelated_target(tmp_path):
    old = _small_app(tmp_path, "dest", "1")
    backup = old.parent / f".trash-{uuid.uuid4()}"
    old.rename(backup)
    old.mkdir()
    (old / "unrelated").write_text("kept")
    app_mod._sweep_installs(old.parent)
    assert (backup / "Contents/Info.plist").is_file()
    assert (old / "unrelated").read_text() == "kept"


@pytest.mark.parametrize("info", [[], "unrelated plist"])
def test_install_sweep_ignores_non_dictionary_plists(tmp_path, info):
    trash = tmp_path / f".trash-{uuid.uuid4()}"
    (trash / "Contents").mkdir(parents=True)
    plist = trash / "Contents/Info.plist"
    plist.write_bytes(plistlib.dumps(info))
    app_mod._sweep_installs(tmp_path)
    assert plistlib.loads(plist.read_bytes()) == info
