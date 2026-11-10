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
