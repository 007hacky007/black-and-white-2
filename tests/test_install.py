import pytest

from bw2 import install, paths
from bw2.install import (GROUP_DEST, UnknownGroup, extract_install, layout_groups,
                         read_manifest, write_manifest)

MANIFEST = paths.REPO / "bw2" / "manifests" / "install-usa.tsv"


def test_manifest_roundtrip(tmp_path):
    root = tmp_path / "root"
    (root / "Data" / "Black & White (x)").mkdir(parents=True)
    (root / "Data" / "Black & White (x)" / "a b.txt").write_bytes(b"abc")
    (root / "top.bin").write_bytes(b"")
    tsv = tmp_path / "m.tsv"
    assert write_manifest(root, tsv) == 2
    m = read_manifest(tsv)
    assert m["Data/Black & White (x)/a b.txt"][0] == 3
    assert m["top.bin"] == (0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")


def test_groups_move_to_their_destinations(tmp_path):
    src = tmp_path / "src"
    for g, f in [("MainApp", "Data/a.txt"), ("White", "white.exe"),
                 ("SplashEnglish", "Splash.tga"), ("_Engine_Kernel_Placeholder", "Setup.dll")]:
        (src / g / f).parent.mkdir(parents=True, exist_ok=True)
        (src / g / f).write_bytes(b"x")
    out = tmp_path / "out"
    layout_groups(src, out)
    files = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    assert files == ["Data/Splash/Splash.tga", "Data/a.txt", "white.exe"]


def test_unknown_group_is_an_error(tmp_path):
    (tmp_path / "src" / "Mystery").mkdir(parents=True)
    (tmp_path / "src" / "Mystery" / "f").write_bytes(b"x")
    with pytest.raises(UnknownGroup, match="Mystery"):
        layout_groups(tmp_path / "src", tmp_path / "out")


@pytest.mark.gamefiles
def test_install_tree_complete():
    from bw2.disc import unpacked_bin
    discs = {n: unpacked_bin(paths.discs_dir() / f"Black & White 2 (USA) (Disc {n}).zip")
             for n in range(1, 5)}
    out = extract_install(discs, paths.GAMEFILES / "install")
    have = {p.relative_to(out).as_posix(): p for p in out.rglob("*") if p.is_file()}
    want = read_manifest(MANIFEST)
    assert set(have) == set(want)
    for rel, (size, sha) in want.items():
        assert have[rel].stat().st_size == size, rel
    assert (out / "white.exe").stat().st_size == 21739061
    from bw2.releases import sha256_file
    bad = [r for r, (s, h) in want.items() if sha256_file(have[r]) != h]
    assert bad == []


def test_extract_install_checks_space_before_writing(tmp_path, monkeypatch):
    import shutil as _sh
    from collections import namedtuple
    from bw2 import disc as _disc
    from bw2.diskspace import NotEnoughSpace
    from fixtures import make_iso
    g = tmp_path / "gamefiles"
    monkeypatch.setattr(install, "GAMEFILES", g)
    monkeypatch.setattr(_disc, "GAMEFILES", g)
    files = {f.upper(): b"x" * 4096 for f in install.IS_FILES}
    iso = make_iso(tmp_path, files)
    Usage = namedtuple("Usage", "total used free")
    monkeypatch.setattr(_sh, "disk_usage", lambda p: Usage(10**12, 0, 10**6))
    with pytest.raises(NotEnoughSpace) as ei:
        extract_install({1: iso}, g / "install")
    assert "need" in str(ei.value)
    assert not g.exists() or not any(p.is_file() for p in g.rglob("*"))
