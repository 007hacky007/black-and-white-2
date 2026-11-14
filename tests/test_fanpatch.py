import pytest

from bw2 import paths
from bw2.download import fetch
from bw2.fanpatch import FAN_PATCH_URL, extract_fan_exe
from bw2.releases import identify, load_releases


@pytest.mark.gamefiles
def test_fan_exe_extracted():
    rel = next(r for r in load_releases() if r.kind == "fanpatch-installer")
    inst = fetch(FAN_PATCH_URL, paths.GAMEFILES / "patches" / rel.name, rel)
    exe = extract_fan_exe(inst, paths.GAMEFILES / "exe")
    assert exe == paths.GAMEFILES / "exe" / "white.exe"
    assert exe.stat().st_size == 20828160


@pytest.mark.gamefiles
def test_extract_without_innoextract(monkeypatch, tmp_path):
    monkeypatch.setenv("PATH", "/usr/bin:/bin")
    rel = next(r for r in load_releases() if r.kind == "fanpatch-installer")
    want = next(r for r in load_releases() if r.kind == "white-exe")
    exe = extract_fan_exe(paths.GAMEFILES / "patches" / rel.name, tmp_path)
    assert exe == tmp_path / "white.exe"
    assert identify(exe, releases=[want]) == want
    assert sorted(p.name for p in tmp_path.iterdir()) == ["white.exe"]
