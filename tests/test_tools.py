import hashlib
import os
import subprocess

from bw2 import build, paths

VENDOR = paths.REPO / "third_party" / "unshield"
SOURCE = paths.REPO / "third_party" / "unshield.SOURCE"


def _listed() -> dict[str, str]:
    out = {}
    for line in SOURCE.read_text().splitlines():
        parts = line.split("  ")
        if len(parts) == 2 and len(parts[0]) == 64 and not line.startswith("#"):
            out[parts[1]] = parts[0]
    return out


def test_unshield_vendored_unchanged():
    listed = _listed()
    have = {p.relative_to(VENDOR).as_posix(): p for p in VENDOR.rglob("*") if p.is_file()}
    assert listed, "unshield.SOURCE lists no files"
    assert set(have) == set(listed)
    for rel, sha in listed.items():
        assert hashlib.sha256(have[rel].read_bytes()).hexdigest() == sha, rel


def test_unshield_builds(tmp_path):
    from bw2 import tools
    exe = tools.unshield(tmp_path)
    assert exe == tmp_path / "unshield"
    assert os.access(exe, os.X_OK)
    r = subprocess.run([str(exe), "-V"], capture_output=True, text=True)
    assert "Unshield version 1.6.2" in r.stdout + r.stderr
    mtime = exe.stat().st_mtime_ns
    assert tools.unshield(tmp_path) == exe
    assert exe.stat().st_mtime_ns == mtime                # unchanged sources: not recompiled
    (tmp_path / "unshield.sha256").write_text("0" * 64 + "\n")
    tools.unshield(tmp_path)
    assert exe.stat().st_mtime_ns != mtime                # stamp differs: recompiled
    assert not (tmp_path / "unshield.part").exists()


def test_unshield_links_only_the_system(tmp_path):
    from bw2 import tools
    exe = tools.unshield(tmp_path)
    r = subprocess.run(["/usr/bin/otool", "-L", str(exe)], capture_output=True, text=True, check=True)
    libs = [l.strip().split(" ")[0] for l in r.stdout.splitlines()[1:] if l.strip()]
    assert libs
    assert [l for l in libs if not l.startswith("/usr/lib/")] == []


def test_unshield_compile_has_sdk(monkeypatch, tmp_path):
    from bw2 import tools
    calls = []

    def fake_run(argv, **kw):
        calls.append(list(argv))
        out = argv[argv.index("-o") + 1]
        with open(out, "wb") as f:
            f.write(b"#!/bin/sh\n")
        return subprocess.CompletedProcess(argv, 0, "", "")
    sdk = build.sdk_path()
    monkeypatch.setattr(build, "sdk_path", lambda: sdk)
    monkeypatch.setattr(build, "check_tools", lambda: paths.REPO / "clang")
    monkeypatch.setattr(tools.subprocess, "run", fake_run)
    tools.unshield(tmp_path)
    [argv] = calls
    assert argv[0] == str(paths.REPO / "clang")
    i = argv.index("-isysroot")
    assert argv[i + 1] == str(sdk)
    assert "-I" in argv and str(VENDOR / "lib") in argv
    assert argv[argv.index(str(VENDOR / "lib")) - 1] == "-I"
    assert "-O2" in argv and "-mcpu=apple-m1" in argv and "-mmacosx-version-min=13.0" in argv
    assert argv[-1] == str(tmp_path / "unshield.part")


def test_unshield_compile_failure_names_clang_output(monkeypatch, tmp_path):
    import pytest
    from bw2 import tools

    def fake_run(argv, **kw):
        return subprocess.CompletedProcess(argv, 1, "", "lib/file.c:1: error: boom\n")
    sdk = build.sdk_path()
    monkeypatch.setattr(build, "sdk_path", lambda: sdk)
    monkeypatch.setattr(build, "check_tools", lambda: paths.REPO / "clang")
    monkeypatch.setattr(tools.subprocess, "run", fake_run)
    with pytest.raises(build.BuildError, match="boom"):
        tools.unshield(tmp_path)
    assert not (tmp_path / "unshield").exists()
    assert not (tmp_path / "unshield.sha256").exists()


SHIM_TEST = r'''
#include <stdio.h>
#include <openssl/evp.h>
int main(void) {
    unsigned char d[16]; unsigned int n = 0;
    if (EVP_MD_CTX_init(NULL) || EVP_DigestInit_ex(NULL, EVP_md5(), NULL)
        || EVP_DigestUpdate(NULL, "a", 1) || EVP_DigestFinal_ex(NULL, d, &n)) return 2;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_MD_CTX_init(c);
    if (!EVP_DigestInit_ex(c, EVP_md5(), NULL)) return 3;
    EVP_DigestUpdate(c, "a", 1);
    EVP_DigestUpdate(c, "bc", 2);
    EVP_DigestUpdate(c, "", 0);
    EVP_DigestFinal_ex(c, d, &n);
    EVP_MD_CTX_free(c);
    for (int i = 0; i < 16; i++) printf("%02x", d[i]);
    printf(" %u\n", n);
    return 0;
}
'''


def test_evp_shim_md5(tmp_path):
    from bw2 import tools
    src = tmp_path / "t.c"
    src.write_text(SHIM_TEST)
    exe = tmp_path / "t"
    subprocess.run([str(build.check_tools()), "-isysroot", str(build.sdk_path()),
                    "-Wno-deprecated-declarations", "-I", str(tools.UNSHIELD_OURS), str(src),
                    "-o", str(exe)], check=True, capture_output=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0
    assert r.stdout.strip() == hashlib.md5(b"abc").hexdigest() + " 16"
