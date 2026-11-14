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


def test_truncated_raw_image_raises_not_hangs(iso, tmp_path):
    cue = iso_to_bincue(iso)
    b = cue.with_suffix(".bin")
    b.write_bytes(b.read_bytes()[: 2352 * 18])
    with open_disc(cue) as d:
        with pytest.raises(DiscFormatError, match="LBA"):
            d.read(17, 3 * 2048)


def test_truncated_iso_extract_raises_and_leaves_nothing(iso, tmp_path):
    with open_disc(iso) as d:
        e = find(list_files(d), "sub/data1.cab")
    iso.write_bytes(iso.read_bytes()[: (e.lba + 2) * 2048])
    dest = tmp_path / "cab"
    with open_disc(iso) as d:
        with pytest.raises(DiscFormatError):
            extract(d, e, dest)
    assert not dest.exists() and not dest.with_name("cab.part").exists()


def test_zips_with_same_inner_names_do_not_collide(tmp_path, gamefiles):
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    a = make_iso(tmp_path / "a", {"A.TXT": b"a" * 3000})
    b = make_iso(tmp_path / "b", {"B.TXT": b"b" * 3000})
    za = zip_bincue(iso_to_bincue(a, stem="game"))
    zb = zip_bincue(iso_to_bincue(b, stem="game"))
    za = za.rename(tmp_path / "Disc A.zip")
    zb = zb.rename(tmp_path / "Disc B.zip")
    with open_disc(za) as d:
        assert find(list_files(d), "a.txt")
    with open_disc(zb) as d:
        assert find(list_files(d), "b.txt")


def test_whole_set_space_checked_before_any_unpack(iso, tmp_path, gamefiles, monkeypatch):
    zips = []
    for n in (1, 2):
        (tmp_path / f"s{n}").mkdir()
        i = make_iso(tmp_path / f"s{n}", {f"F{n}.TXT": bytes([n]) * 5000})
        zips.append(zip_bincue(iso_to_bincue(i, stem=f"d{n}")))
    import zipfile
    sizes = [sum(i.file_size for i in zipfile.ZipFile(z).infolist()) for z in zips]
    Usage = namedtuple("Usage", "total used free")
    # the first disc alone fits, the set does not
    monkeypatch.setattr(shutil, "disk_usage", lambda p: Usage(10**12, 0, sizes[0] + 1))
    with pytest.raises(NotEnoughSpace) as ei:
        identify_discs(zips, releases=[])
    assert f"need {sum(sizes)} bytes" in str(ei.value)
    assert not gamefiles.exists() or not any(p.is_file() for p in gamefiles.rglob("*"))


@pytest.mark.gamefiles
def test_iso_is_known(tmp_path):
    from bw2.releases import identify
    b = disc.unpacked_bin(_real_zip(4))
    iso = tmp_path / "disc4.iso"
    with open(b, "rb") as src, open(iso, "wb") as dst:
        while buf := src.read(disc.RAW * 4096):
            assert len(buf) % disc.RAW == 0
            for i in range(0, len(buf), disc.RAW):
                dst.write(buf[i + disc.RAW_DATA_OFFSET:i + disc.RAW_DATA_OFFSET + disc.COOKED])
    rel = identify(iso, kind="disc")
    assert rel is not None and rel.name == "Black & White 2 (USA) (Disc 4)"
    assert rel.note == "iso: 2048-byte user data of the redump bin"
    with open_disc(iso) as a, open_disc(b) as c:
        assert list_files(a) == list_files(c)


@pytest.mark.gamefiles
def test_zip_table():
    import zipfile
    from bw2.builder_cli import load_zip_table
    rows = load_zip_table()
    assert sorted(rows) == [1, 2, 3, 4]
    for n in range(1, 5):
        with zipfile.ZipFile(_real_zip(n)) as zf:
            bins = [i for i in zf.infolist() if i.filename.lower().endswith(".bin")]
        assert len(bins) == 1
        assert rows[n] == (bins[0].filename, bins[0].file_size, bins[0].CRC)
