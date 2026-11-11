"""Apply an RTPatch diff by running the patchw32.dll that ships with it, under Unicorn.

The DLL reports through a callback: id 7 names the file it is working on, id 9
is a line of its log, ids 3 and 4 are an error. Its end-of-run summary counts
files modified, added, deleted, renamed, skipped (already new), and ignored
(old file missing or invalid). An ignored file is a failure here: it means the
1.0 tree is not the one the patch was made for.
"""
import re
import time
from dataclasses import dataclass, field
from pathlib import Path

from .pe32emu import GuestException, Pe32Host, Win32Unimplemented
from .w32base import install_base
from .w32files import install_files

_COUNT = re.compile(r"(Modified|Renamed|Added|Deleted|Up-To-Date|Missing|Invalid)\b[^(\n]*\(\s*(\d+)\s*\)")
_FIELDS = {"Modified": "modified", "Renamed": "renamed", "Added": "added", "Deleted": "deleted",
           "Up-To-Date": "skipped", "Missing": "missing", "Invalid": "invalid"}
_SUMMARY_LINE = re.compile(r"\(\s*\d+\s*\)")
_VERIFIED = re.compile(r"verif", re.I)
_TROUBLE = re.compile(r"invalid|missing|not found|skipp|ignor", re.I)
CB_ERROR_PREFIX, CB_ERROR_TEXT, CB_FILE, CB_MESSAGE = 3, 4, 7, 9


class RtpError(Exception):
    pass


@dataclass
class RtpResult:
    modified: int = 0
    added: int = 0
    deleted: int = 0
    renamed: int = 0
    skipped: int = 0
    missing: int = 0
    invalid: int = 0
    files: list = field(default_factory=list)
    seconds: float = 0.0


def parse_summary(lines: list[str]) -> RtpResult:
    r = RtpResult()
    for line in lines:
        for word, n in _COUNT.findall(line):
            setattr(r, _FIELDS[word], int(n))
    return r


def check_run(events: list[tuple[int, str]], ret: int) -> RtpResult:
    """Counts from a run's callback events; raises RtpError if the run did not go cleanly."""
    r = parse_summary([t for i, t in events if i == CB_MESSAGE])
    r.files = [t for i, t in events if i == CB_FILE]
    errors = "".join(t for i, t in events if i in (CB_ERROR_PREFIX, CB_ERROR_TEXT)).strip()
    if errors:
        raise RtpError(f"RTPatch error: {errors}")
    if ret != 0:
        raise RtpError(f"RTPatchApply32 returned {ret}")
    if r.missing or r.invalid:
        # A file the DLL skips gets its "Applying" line and no verification line (seen on
        # a clone with one corrupted 1.0 file); every file it patches is verified.
        blocks: dict[str, list[str]] = {}
        current = None
        for i, t in events:
            if i == CB_MESSAGE and _SUMMARY_LINE.search(t):
                break                                  # the summary's own lines name no file
            if i == CB_FILE:
                current = t
                blocks.setdefault(t, [])
            elif i == CB_MESSAGE and current:
                blocks[current].append(t)
        bad = [f for f, msgs in blocks.items()
               if not any(_VERIFIED.search(m) for m in msgs) or any(_TROUBLE.search(m) for m in msgs)]
        raise RtpError(f"{r.missing} old file(s) missing and {r.invalid} invalid; "
                       f"the 1.0 tree does not match the patch: {', '.join(bad) or 'see the log'}")
    return r


def apply_rtp(dll: Path, rtp: Path, tree: Path, log: Path | None = None) -> RtpResult:
    t0 = time.time()
    dll, rtp, tree = Path(dll), Path(rtp), Path(tree)
    host = Pe32Host(dll)
    fs = install_files(host, {"P": rtp.parent, "T": tree}, writable={"T"})
    install_base(host, command_line="patch.exe", module_path=f"P:\\{dll.name}")
    events: list[tuple[int, str]] = []
    empty = host.virtual_alloc(16)

    def callback(cb_id: int, param: int) -> int:
        if cb_id in (CB_ERROR_PREFIX, CB_ERROR_TEXT, CB_FILE, CB_MESSAGE) and param:
            events.append((cb_id, host.cstr(param) or ""))
        return empty                                   # non-NULL: carry on

    cb = host.callback(callback, 2)
    cmd = host.virtual_alloc(0x400)
    host.put_str(cmd, f'"P:\\{rtp.name}" T:\\')
    ret = None
    try:
        entry = host.base + host.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        if not host.call(entry, host.base, 1, 0):
            raise RtpError(f"{dll.name}: DllMain failed")
        ret = host.call(host.export("RTPatchApply32@12"), cmd, cb, 1)
    except (Win32Unimplemented, GuestException) as e:
        last = next((t for i, t in reversed(events) if i == CB_FILE), "before the first file")
        raise RtpError(f"{rtp.name}: {e} (while on {last})") from e
    finally:
        fs.shutdown()                                  # as process exit: write back, close files
        if log is not None:
            Path(log).write_text("".join(f"{i}\t{t}\n" if not t.endswith("\n") else f"{i}\t{t}"
                                         for i, t in events))
    if fs.refused:
        raise RtpError(f"{rtp.name} tried paths outside the tree: {', '.join(fs.refused[:5])}")
    r = check_run(events, ret)
    r.seconds = round(time.time() - t0, 1)
    return r
