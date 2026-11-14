"""Downloads pinned by size and SHA-256. Nothing half-written ever looks valid."""
import html
import http.cookiejar
import re
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from .releases import Release, identify

USER_AGENT = "bw2-builder/0"

# PCGamingWiki's file pages for the two official patches and the r= id of each file ("Measured" item 7).
PATCH_SOURCES = {
    "patch-1.1": ("https://community.pcgamingwiki.com/files/file/2151-black-white-2-v11-patch/", 10345),
    "patch-1.2": ("https://community.pcgamingwiki.com/files/file/2152-black-white-2-v12-patch/", 10346),
}


class FingerprintMismatch(Exception):
    pass


class DownloadBlocked(Exception):
    """The site did not hand out the file: a bot challenge, or a page without the expected link."""


def _save(opener, url: str, dest: Path, expected: Release, progress=None) -> Path:
    """Downloads url to dest.part, checks size and SHA-256, renames; progress(fraction) after each chunk."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with opener(req, timeout=60) as r, open(part, "wb") as f:
            done = 0
            while chunk := r.read(1 << 20):
                f.write(chunk)
                done += len(chunk)
                if progress:
                    progress(min(1.0, done / expected.size) if expected.size else 1.0)
        if not identify(part, releases=[expected]):
            raise FingerprintMismatch(f"{url}: download does not match {expected.name} "
                                      f"({expected.size} bytes, sha256 {expected.sha256})")
        part.replace(dest)
        if progress:
            progress(1.0)
        return dest
    finally:
        part.unlink(missing_ok=True)


def fetch(url: str, dest: Path, expected: Release, progress=None) -> Path:
    dest = Path(dest)
    if dest.exists() and identify(dest, releases=[expected]):
        return dest
    return _save(urllib.request.urlopen, url, dest, expected, progress)


def _page(open_url, url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with open_url(req, timeout=60) as r:
            return r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        if e.headers.get("cf-mitigated") == "challenge" or "Just a moment" in e.read(4096).decode(errors="replace"):
            raise DownloadBlocked(f"{url}: the site answered with a bot challenge (HTTP {e.code}); download the "
                                  f"file in a browser and choose it with 'I have it'") from e
        raise DownloadBlocked(f"{url}: HTTP {e.code} {e.reason}") from e


def fetch_pcgw(page: str, r: int, dest: Path, expected: Release, progress=None) -> Path:
    """A file from PCGamingWiki's community files: the download page (a session cookie and a csrfKey), the
    terms confirmation (the file's r=<id>&confirm=1&t=1 link), then that link, which redirects to the file."""
    dest = Path(dest)
    if dest.exists() and identify(dest, releases=[expected]):
        return dest
    jar = http.cookiejar.CookieJar()
    open_url = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar)).open
    first = f"{page}?do=download"
    text = html.unescape(_page(open_url, first))
    m = re.search(r"csrfKey=([0-9A-Za-z]+)", text) or \
        re.search(r"""csrfKey["']?\s*[:,=]\s*["']([0-9A-Za-z]+)["']""", text)
    if not m:
        raise DownloadBlocked(f"{first}: no csrfKey on the download page")
    key = m.group(1)
    second = f"{page}?do=download&confirm=1&csrfKey={key}"
    text = html.unescape(_page(open_url, second))
    want = f"r={r}&confirm=1&t=1"
    links = [urllib.parse.urljoin(second, h) for h in re.findall(r"""href=["']([^"']+)["']""", text)
             if want in h and f"csrfKey={key}" in h]
    links = [u for u in links if urllib.parse.urlsplit(u).netloc == urllib.parse.urlsplit(page).netloc]
    if not links:
        raise DownloadBlocked(f"{second}: no link with {want}&csrfKey=... on the confirmation page")
    return _save(open_url, links[0], dest, expected, progress)
