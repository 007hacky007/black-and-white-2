import hashlib
import http.server
import threading

import pytest

from bw2.download import FingerprintMismatch, fetch
from bw2.releases import Release

DATA = b"payload " * 1000


@pytest.fixture
def server(tmp_path):
    (tmp_path / "srv").mkdir()
    (tmp_path / "srv" / "f.exe").write_bytes(DATA)
    seen = {"hits": 0, "ua": None}

    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=str(tmp_path / "srv"), **k)

        def do_GET(self):
            seen["hits"] += 1
            seen["ua"] = self.headers.get("User-Agent")
            super().do_GET()

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_port}/f.exe", seen
    httpd.shutdown()


def _rel(data=DATA):
    return Release("patch", "f", len(data), hashlib.sha256(data).hexdigest(), "")


def test_fetch_ok(server, tmp_path):
    url, _ = server
    dest = tmp_path / "dl" / "f.exe"
    assert fetch(url, dest, _rel()) == dest
    assert dest.read_bytes() == DATA


def test_fetch_bad_hash_leaves_nothing(server, tmp_path):
    url, _ = server
    dest = tmp_path / "dl" / "f.exe"
    wrong = Release("patch", "f", len(DATA), "0" * 64, "")
    with pytest.raises(FingerprintMismatch):
        fetch(url, dest, wrong)
    assert not dest.exists()
    assert not dest.with_name("f.exe.part").exists()


def test_fetch_reuses_existing(server, tmp_path):
    url, seen = server
    dest = tmp_path / "f.exe"
    dest.write_bytes(DATA)
    fetch(url, dest, _rel())
    assert seen["hits"] == 0


def test_fetch_sends_generic_user_agent(server, tmp_path):
    url, seen = server
    fetch(url, tmp_path / "f.exe", _rel())
    assert seen["ua"] == "bw2-builder/0"
