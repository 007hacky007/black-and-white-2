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


def test_fetch_progress(server, tmp_path):
    url, _ = server
    seen = []
    fetch(url, tmp_path / "f.exe", _rel(), progress=seen.append)
    assert seen and seen == sorted(seen) and seen[-1] == 1.0 and all(0 < f <= 1 for f in seen)


PCGW_PAGE = "/files/file/2152-black-white-2-v12-patch/"


@pytest.fixture
def pcgw(tmp_path):
    """A fake of PCGamingWiki's three download steps ("Measured" item 7): ?do=download sets a session cookie
    and shows a csrfKey; &confirm=1&csrfKey=K needs the cookie and shows the r=...&confirm=1&t=1 link; that link
    redirects to the file."""
    state = {"data": DATA, "challenge": False, "log": []}

    class H(http.server.BaseHTTPRequestHandler):
        def _send(self, code, body=b"", headers=()):
            self.send_response(code)
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path, _, query = self.path.partition("?")
            q = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
            cookie = self.headers.get("Cookie", "")
            state["log"].append((path, q, cookie, self.headers.get("User-Agent")))
            if state["challenge"]:
                return self._send(403, b"<title>Just a moment...</title>", [("cf-mitigated", "challenge")])
            if path == "/real/BW2Patch1_2.exe":
                return self._send(200, state["data"])
            if path != PCGW_PAGE or q.get("do") != "download":
                return self._send(404)
            if "confirm" not in q:
                body = (b'<html><script>ips.setSetting("csrfKey", "a1b2c3");</script>'
                        b'<a href="' + PCGW_PAGE.encode() + b'?do=download&amp;confirm=1&amp;csrfKey=a1b2c3">'
                        b'Agree</a></html>')
                return self._send(200, body, [("Set-Cookie", "ips4_session=s123; path=/")])
            if "ips4_session=s123" not in cookie or q.get("csrfKey") != "a1b2c3":
                return self._send(403, b"no session")
            if q.get("r") == "10346" and q.get("t") == "1":
                return self._send(301, b"", [("Location", "/real/BW2Patch1_2.exe")])
            body = (b'<a href="https://elsewhere.invalid/?do=download&amp;r=1&amp;confirm=1&amp;t=1">x</a>'
                    b'<a href="' + PCGW_PAGE.encode() + b'?do=download&amp;r=10346&amp;confirm=1&amp;t=1&amp;'
                    b'csrfKey=a1b2c3" class="ipsButton">Download</a>')
            return self._send(200, body)

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_port}{PCGW_PAGE}", state
    httpd.shutdown()


def test_fetch_pcgw(pcgw, tmp_path):
    from bw2.download import fetch_pcgw
    page, state = pcgw
    dest = tmp_path / "dl" / "BW2Patch1_2.exe"
    seen = []
    assert fetch_pcgw(page, 10346, dest, _rel(), progress=seen.append) == dest
    assert dest.read_bytes() == DATA and seen[-1] == 1.0
    steps = [(p, q.get("confirm"), q.get("r")) for p, q, _, _ in state["log"]]
    assert steps == [(PCGW_PAGE, None, None), (PCGW_PAGE, "1", None), (PCGW_PAGE, "1", "10346"),
                     ("/real/BW2Patch1_2.exe", None, None)]
    assert {ua for *_, ua in state["log"]} == {"bw2-builder/0"}
    # a server that serves other bytes leaves no file
    state["data"] = DATA[:-1] + b"!"
    other = tmp_path / "dl2" / "BW2Patch1_2.exe"
    with pytest.raises(FingerprintMismatch):
        fetch_pcgw(page, 10346, other, _rel())
    assert not other.exists() and not other.with_name(other.name + ".part").exists()


def test_fetch_pcgw_challenge_is_named(pcgw, tmp_path):
    from bw2.download import DownloadBlocked, fetch_pcgw
    page, state = pcgw
    state["challenge"] = True
    dest = tmp_path / "BW2Patch1_2.exe"
    with pytest.raises(DownloadBlocked) as ei:
        fetch_pcgw(page, 10346, dest, _rel())
    assert "challenge" in str(ei.value) and page in str(ei.value)
    assert not dest.exists()


def test_fetch_pcgw_no_link_is_named(pcgw, tmp_path):
    from bw2.download import DownloadBlocked, fetch_pcgw
    page, _ = pcgw
    with pytest.raises(DownloadBlocked) as ei:
        fetch_pcgw(page, 99999, tmp_path / "x.exe", _rel())
    assert "r=99999" in str(ei.value)
