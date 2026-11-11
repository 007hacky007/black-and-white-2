import subprocess

import pytest

from bw2 import paths
from bw2.rtpatch import RtpError, check_run, parse_summary

# Synthetic end-of-run lines in the DLL's layout: a keyword, dots, a count in parentheses.
SUMMARY = ["   Files Modified ........ (   3)\n", "   Files Renamed. ........ (0)\n",
           "   Files Added ........... (1)\n", "   Files Deleted ......... (2)\n",
           "Skipped: New Files Up-To-Date .... (4)\n",
           "Ignored: Old Files Missing ....... (0)\n", "         Old Files Invalid ....... (0)\n"]


def test_parse_summary_counts():
    r = parse_summary(SUMMARY)
    assert (r.modified, r.renamed, r.added, r.deleted, r.skipped, r.missing, r.invalid) == \
        (3, 0, 1, 2, 4, 0, 0)


def test_summary_with_ignored_files_fails():
    lines = [l.replace("Invalid ....... (0)", "Invalid ....... (1)") for l in SUMMARY]
    # Real shape (corrupted-clone run): a skipped file gets its "Applying" line and no
    # verification line; patched files get one.
    events = [(7, "Data\\good.sdv"), (9, "Applying the patch\n"), (9, "    Verifying - ok\n"),
              (7, "Data\\bad.sdv"), (9, "Applying the patch\n"), (9, "\n")] + [(9, l) for l in lines]
    with pytest.raises(RtpError, match=r"bad\.sdv"):
        check_run(events, 0)


def test_error_message_fails():
    events = [(3, "\nerror ept4.4d: "), (4, "Something went wrong\n")] + [(9, l) for l in SUMMARY]
    with pytest.raises(RtpError, match="Something went wrong"):
        check_run(events, 0)


def test_nonzero_return_fails():
    with pytest.raises(RtpError, match="returned 5"):
        check_run([(9, l) for l in SUMMARY], 5)


def test_clean_run_passes():
    r = check_run([(7, "a"), (7, "b")] + [(9, l) for l in SUMMARY], 0)
    assert r.files == ["a", "b"] and r.modified == 3


@pytest.fixture(scope="module")
def patched_11(tmp_path_factory):
    from bw2.patches import patch_inputs
    from bw2.rtpatch import apply_rtp
    work = tmp_path_factory.mktemp("rtp")
    tree = work / "tree"
    subprocess.run(["/bin/cp", "-cR", str(paths.GAMEFILES / "install"), str(tree)], check=True)
    pi = patch_inputs(paths.GAMEFILES / "patches" / "BW2Patch1_1.exe", "patch-1.1",
                      paths.GAMEFILES / "patches" / "inputs-1_1")
    return tree, apply_rtp(pi.dll, pi.rtp, tree, log=work / "rtp-1.1.log")


@pytest.mark.gamefiles
def test_apply_rtp_real_1_1(patched_11):
    _, r = patched_11
    assert r.modified == 69 and r.missing == r.invalid == 0
    print(f"rtp 1.1: {r.seconds} s")


@pytest.mark.gamefiles
def test_apply_rtp_real_1_2(patched_11, tmp_path):
    from bw2.patches import patch_inputs
    from bw2.rtpatch import apply_rtp
    tree, _ = patched_11
    pi = patch_inputs(paths.GAMEFILES / "patches" / "BW2Patch1_2.exe", "patch-1.2",
                      paths.GAMEFILES / "patches" / "inputs-1_2")
    r = apply_rtp(pi.dll, pi.rtp, tree, log=tmp_path / "rtp-1.2.log")
    assert r.modified == 8 and r.added == 1 and r.missing == r.invalid == 0
    print(f"rtp 1.2: {r.seconds} s")


def test_apply_rtp_always_shuts_files_down(tmp_path, monkeypatch):
    # Review: views patchw32 never unmaps kept files open until garbage collection.
    import bw2.rtpatch as rt
    from bw2.pe32emu import Win32Unimplemented

    class FakeHost:
        base = 0x10000000

        class pe:
            class OPTIONAL_HEADER:
                AddressOfEntryPoint = 0x1000

        def __init__(self, dll):
            pass

        def virtual_alloc(self, n):
            return 0x20000000

        def callback(self, fn, n):
            return 0x7F000000

        def put_str(self, va, s, wide=False):
            return len(s)

        def call(self, *a):
            raise Win32Unimplemented("unimplemented Win32 import Nope")

    class FakeFs:
        refused = []
        shut = 0

        def shutdown(self):
            FakeFs.shut += 1

    monkeypatch.setattr(rt, "Pe32Host", FakeHost)
    monkeypatch.setattr(rt, "install_files", lambda h, roots, writable: FakeFs())
    monkeypatch.setattr(rt, "install_base", lambda h, **k: None)
    with pytest.raises(RtpError, match="Nope"):
        rt.apply_rtp(tmp_path / "patchw32.dll", tmp_path / "x.RTP", tmp_path)
    assert FakeFs.shut == 1


def test_bad_file_named_not_the_last_one():
    # Review: the summary's own "Ignored ... Missing/Invalid" lines blamed the last file.
    lines = [l.replace("Invalid ....... (0)", "Invalid ....... (1)") for l in SUMMARY]
    events = [(7, "Data\\first.sdv"), (9, "Verifying - ok\n"), (7, "Data\\bad.sdv"),
              (9, "Applying the patch\n"), (7, "Data\\last.sdv"), (9, "Verifying - ok\n")] + \
        [(9, "File Patches Applied ......... (  2)\n")] + [(9, l) for l in lines]
    with pytest.raises(RtpError) as e:
        check_run(events, 0)
    assert "bad.sdv" in str(e.value) and "last.sdv" not in str(e.value)


@pytest.mark.gamefiles
def test_corrupted_old_file_is_named(tmp_path):
    from bw2.patches import patch_inputs
    from bw2.rtpatch import apply_rtp
    tree = tmp_path / "tree"
    subprocess.run(["/bin/cp", "-cR", str(paths.GAMEFILES / "install"), str(tree)], check=True)
    victim = tree / "Data" / "effects" / "yard.sdv"
    victim.unlink()                                   # break the clone, not the original
    victim.write_bytes(b"not the 1.0 file")
    pi = patch_inputs(paths.GAMEFILES / "patches" / "BW2Patch1_1.exe", "patch-1.1",
                      paths.GAMEFILES / "patches" / "inputs-1_1")
    with pytest.raises(RtpError, match=r"yard\.sdv"):
        apply_rtp(pi.dll, pi.rtp, tree, log=tmp_path / "rtp.log")
