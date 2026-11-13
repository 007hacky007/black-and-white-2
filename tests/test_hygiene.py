import subprocess
from bw2 import pcr, paths


def test_game_material_is_ignored():
    for name in ["Black & White 2 (USA) (Disc 1).zip", "x.bin", "x.cue", "x.iso",
                 "white.exe", "gamefiles/install/a.txt", "out/a.c", "gen/a.c", "a.part"]:
        r = subprocess.run(["git", "check-ignore", "-q", name], cwd=paths.REPO)
        assert r.returncode == 0, name


def test_pcrecomp_runs_and_audit_is_clean():
    assert pcr.run("tools/audit_repo.py", "--selftest").returncode == 0
    r = pcr.run("tools/audit_repo.py", str(paths.REPO), check=False)
    assert r.returncode == 0, r.stdout + r.stderr


def test_no_windows_binary_is_tracked():
    # the Windows probe (tools/winprobe) is built into out/ and never committed
    r = subprocess.run(["git", "ls-files", "-z"], cwd=paths.REPO, capture_output=True, text=True, check=True)
    tracked = [n for n in r.stdout.split("\0") if n.lower().endswith((".exe", ".dll"))]
    assert tracked == []
    assert subprocess.run(["git", "check-ignore", "-q", "out/winprobe/saveprobe.exe"], cwd=paths.REPO).returncode == 0
