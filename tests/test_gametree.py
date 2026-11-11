import shutil
from collections import namedtuple

import pytest

from bw2 import gametree, paths
from bw2.diskspace import NotEnoughSpace
from bw2.install import write_manifest
from bw2.releases import identify


def test_copy_groups_case_insensitive_destinations(tmp_path):
    tree, groups = tmp_path / "tree", tmp_path / "groups"
    (tree / "data" / "effects").mkdir(parents=True)
    (tree / "data" / "effects" / "skinned.sdv").write_bytes(b"old")
    (groups / "ATI_Fix").mkdir(parents=True)
    (groups / "ATI_Fix" / "Skinned.sdv").write_bytes(b"new")
    (groups / "Readme_English").mkdir()
    (groups / "Readme_English" / "readme_v1_2.txt").write_bytes(b"r")
    copied, new = gametree.copy_groups(groups, [("ATI_Fix", "Data/Effects"), ("Readme_English", "")], tree)
    assert (tree / "data" / "effects" / "skinned.sdv").read_bytes() == b"new"
    assert not (tree / "data" / "effects" / "Skinned.sdv").exists() or \
        sorted(p.name for p in (tree / "data" / "effects").iterdir()) == ["skinned.sdv"]
    assert copied == ["data/effects/skinned.sdv", "readme_v1_2.txt"]
    assert new == {"readme_v1_2.txt"}


def test_copy_fallback_checks_space(tmp_path, monkeypatch):
    src = tmp_path / "install"
    src.mkdir()
    (src / "big.bin").write_bytes(b"\0" * 4096)
    dst = tmp_path / "game.part"
    monkeypatch.setattr(gametree, "_clonefile", lambda a, b: False)
    Usage = namedtuple("Usage", "total used free")
    monkeypatch.setattr(shutil, "disk_usage", lambda p: Usage(10**9, 10**9 - 100, 100))
    with pytest.raises(NotEnoughSpace, match="4096"):
        gametree.clone_tree(src, dst)
    assert not dst.exists()


def test_interrupted_build_not_reused(tmp_path, monkeypatch):
    install, out = tmp_path / "install", tmp_path / "game"
    install.mkdir()
    (install / "white.exe").write_bytes(b"MZ")
    stale = tmp_path / "game.part"
    stale.mkdir()
    (stale / "half-patched.bin").write_bytes(b"x")

    def boom(*a, **k):
        raise RuntimeError("boom after the clone")
    monkeypatch.setattr(gametree, "patch_inputs", boom)
    with pytest.raises(RuntimeError, match="boom"):
        gametree.build_game_tree(install, tmp_path / "p11", tmp_path / "p12", tmp_path / "fan",
                                 out=out, work=tmp_path / "work")
    assert not out.exists()
    assert not (stale / "half-patched.bin").exists()


def test_manifest_mismatch_fails(tmp_path):
    root = tmp_path / "tree"
    (root / "Data").mkdir(parents=True)
    (root / "Data" / "a.txt").write_bytes(b"abc")
    (root / "b.txt").write_bytes(b"b")
    tsv = tmp_path / "m.tsv"
    write_manifest(root, tsv)
    gametree.verify_manifest(root, tsv)
    (root / "Data" / "a.txt").write_bytes(b"abd")
    with pytest.raises(gametree.ManifestMismatch, match="Data/a.txt"):
        gametree.verify_manifest(root, tsv)
    (root / "Data" / "a.txt").write_bytes(b"abc")
    (root / "extra.txt").write_bytes(b"")
    with pytest.raises(gametree.ManifestMismatch, match="extra.txt"):
        gametree.verify_manifest(root, tsv)


@pytest.mark.gamefiles
def test_build_game_tree_real(tmp_path):
    p = paths.GAMEFILES / "patches"
    out = gametree.build_game_tree(paths.GAMEFILES / "install", p / "BW2Patch1_1.exe",
                                   p / "BW2Patch1_2.exe", p / "B&W2 Fan Patch v1.42 Installer.exe",
                                   out=tmp_path / "game")
    assert identify(out / "white.exe", kind="white-exe") is not None
