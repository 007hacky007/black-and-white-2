import hashlib
import shutil
from collections import namedtuple

import pytest

from bw2 import disc, paths
from bw2.diskspace import NotEnoughSpace
from bw2.disc import (DiscFormatError, DiscSetError, extract, find, identify_discs,
                      list_files, open_disc)
from bw2.releases import load_releases
from fixtures import STEM, iso_to_bincue, make_iso, zip_bincue

FILES = {"DATA1.HDR": b"h" * 5000, "SUB/DATA1.CAB": bytes(range(256)) * 274}


@pytest.fixture
def iso(tmp_path):
    return make_iso(tmp_path, FILES)


@pytest.fixture
def gamefiles(tmp_path, monkeypatch):
    g = tmp_path / "gamefiles"
    monkeypatch.setattr(disc, "GAMEFILES", g)
    return g


def _read_all(image, tmp_path):
    with open_disc(image) as d:
        entries = list_files(d)
        out = {}
        for name in ["data1.hdr", "sub/data1.cab"]:
            e = find(entries, name)
            dest = tmp_path / "out" / image.suffix.strip(".") / name
            extract(d, e, dest)
            out[name] = dest.read_bytes()
        return out


def test_iso_lists_and_extracts(iso, tmp_path):
    with open_disc(iso) as d:
        entries = list_files(d)
        assert find(entries, "data1.hdr") is not None
        assert find(entries, "sub/data1.cab").size == 70144
        dest = tmp_path / "x.cab"
        extract(d, find(entries, "SUB/data1.cab"), dest)
        assert dest.read_bytes() == FILES["SUB/DATA1.CAB"]


def test_bincue_and_bin_and_zip_read_same_files(iso, tmp_path, gamefiles):
    want = {"data1.hdr": FILES["DATA1.HDR"], "sub/data1.cab": FILES["SUB/DATA1.CAB"]}
    cue = iso_to_bincue(iso)
    z = zip_bincue(cue)
    assert _read_all(iso, tmp_path) == want
    assert _read_all(cue, tmp_path) == want
    assert _read_all(cue.with_suffix(".bin"), tmp_path) == want
    assert _read_all(z, tmp_path) == want


def test_cue_with_two_tracks_is_rejected(iso):
    cue = iso_to_bincue(iso)
    cue.write_text(cue.read_text() + '  TRACK 02 AUDIO\n    INDEX 01 10:00:00\n')
    with pytest.raises(DiscFormatError):
        open_disc(cue)


def test_mode2_is_rejected(iso):
    cue = iso_to_bincue(iso, mode="MODE2/2352")
    with pytest.raises(DiscFormatError):
        open_disc(cue)


def test_zip_unpack_checks_space(iso, gamefiles, monkeypatch):
    z = zip_bincue(iso_to_bincue(iso))
    Usage = namedtuple("Usage", "total used free")
    monkeypatch.setattr(shutil, "disk_usage", lambda p: Usage(100, 90, 10))
    with pytest.raises(NotEnoughSpace) as ei:
        open_disc(z)
    assert "10" in str(ei.value)
    leftovers = [p for p in gamefiles.rglob("*") if p.is_file()] if gamefiles.exists() else []
    assert leftovers == []


def test_identify_discs_reports_missing_and_duplicates(tmp_path):
    rows, files = [], {}
    for n in range(1, 5):
        data = f"disc {n}".encode() * 10
        p = tmp_path / f"d{n}.bin"
        p.write_bytes(data)
        files[n] = p
        rows.append(f"disc\tBlack & White 2 (USA) (Disc {n})\t{len(data)}\t"
                    f"{hashlib.sha256(data).hexdigest()}\t")
    tsv = tmp_path / "rel.tsv"
    tsv.write_text("\n".join(rows) + "\n")
    rel = load_releases(tsv)
    assert identify_discs(list(files.values()), releases=rel) == files
    with pytest.raises(DiscSetError) as ei:
        identify_discs([files[1], files[2], files[2]], releases=rel)
    assert "missing: 3, 4" in str(ei.value)
    assert "duplicate: 2" in str(ei.value)
    stranger = tmp_path / "other.bin"
    stranger.write_bytes(b"nope")
    with pytest.raises(DiscSetError) as ei:
        identify_discs([*files.values(), stranger], releases=rel)
    assert "other.bin" in str(ei.value)


def _real_zip(n):
    return paths.discs_dir() / f"Black & White 2 (USA) (Disc {n}).zip"


@pytest.mark.gamefiles
def test_real_disc1_has_installshield():
    with open_disc(_real_zip(1)) as d:
        assert find(list_files(d), "data1.hdr") is not None


@pytest.mark.gamefiles
def test_real_discs_identify():
    bins = [disc.unpacked_bin(_real_zip(n)) for n in range(1, 5)]
    assert sorted(identify_discs(bins)) == [1, 2, 3, 4]
