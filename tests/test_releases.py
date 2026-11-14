import hashlib

from bw2.releases import Release, identify, load_releases, sha256_file


def _write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return p


def _tsv(tmp_path, rows):
    p = tmp_path / "rel.tsv"
    p.write_text("# kind\tname\tsize\tsha256\tnote\n" +
                 "".join("\t".join(map(str, r)) + "\n" for r in rows))
    return load_releases(p)


def test_identify_matches_size_and_hash(tmp_path):
    f = _write(tmp_path, "a.bin", b"hello")
    rel = _tsv(tmp_path, [("disc", "A", 5, hashlib.sha256(b"hello").hexdigest(), "")])
    assert sha256_file(f) == hashlib.sha256(b"hello").hexdigest()
    assert identify(f, releases=rel) == rel[0]


def test_identify_rejects_same_size_other_hash(tmp_path):
    f = _write(tmp_path, "a.bin", b"hellp")
    rel = _tsv(tmp_path, [("disc", "A", 5, hashlib.sha256(b"hello").hexdigest(), "")])
    assert identify(f, releases=rel) is None


def test_identify_filters_by_kind(tmp_path):
    f = _write(tmp_path, "a.bin", b"hello")
    rel = _tsv(tmp_path, [("patch", "P", 5, hashlib.sha256(b"hello").hexdigest(), "")])
    assert identify(f, kind="disc", releases=rel) is None
    assert identify(f, kind="patch", releases=rel).name == "P"


def test_bundled_tsv_has_fan_patch():
    rel = load_releases()
    assert Release("fanpatch-installer", "B&W2 Fan Patch v1.42 Installer.exe", 20266576,
                   "2bb875a10b8e10eaca2fcaee8be0e91bf037cda3af161e2700834e023a237bec",
                   "Inno Setup 6.0.0") in rel
    exe = [r for r in rel if r.kind == "white-exe"]
    assert [(r.name, r.size, r.sha256) for r in exe] == [
        ("white.exe 1.42", 20828160,
         "ca745211ac9c63c439493ff14fa27507c4b99d199af4fd2b48577bccfd851db2")]


def test_bundled_tsv_rows():
    rel = load_releases()
    kinds = {}
    for r in rel:
        kinds[r.kind] = kinds.get(r.kind, 0) + 1
    assert len(rel) == 17
    assert kinds["disc"] == 8
    iso = {r.name: (r.size, r.sha256) for r in rel
           if r.kind == "disc" and r.note == "iso: 2048-byte user data of the redump bin"}
    assert iso == {  # "Measured" item 8
        "Black & White 2 (USA) (Disc 1)": (686921728, "47f9bea2e839867ae241761da872e5bd82a7b018f8dd4749f93ec1244b14fa17"),
        "Black & White 2 (USA) (Disc 2)": (681406464, "65e33e5e681be91ed4040805a67daa193db5e12d0aad00b9a37db510fd9489ad"),
        "Black & White 2 (USA) (Disc 3)": (681406464, "b5731d13f45a914fc98d736a6f60be98250d98ba2ccf33f60b3162e6856c30c0"),
        "Black & White 2 (USA) (Disc 4)": (568465408, "dc1652305a24bb65fc2635b7620c1aba970fb4b033233070f06b26457858979e"),
    }
    assert len({(r.size, r.sha256) for r in rel}) == len(rel)
