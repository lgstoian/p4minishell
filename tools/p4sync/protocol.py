# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync wire contract (single source of truth).

Both the CLI (``tools/p4sync``) and the Windows GUI bridge import these
constants. Nothing here talks to hardware: it only names the contract the
current firmware already exposes through ``sync status`` and the SDFX/PIMX/BMPX
frames. No firmware change is required to use this module.

Protocol shape:
    host -> `sync status`
    device -> `sync.<key>: value` capability lines (see CAP_KEYS)
    host -> `receive <path>` / `send <path>` per file (SDFX, shared sdbridge)
    host -> `pim get|put ...` (PIMX, shared pim_sync)
    host -> `screenshot` (BMPX, shared p4test.screenshot)

The firmware prints a ``sync.`` prefix and slightly different names than the
original prototype expected. :func:`normalize_caps` maps those spellings to one
canonical set; :func:`require_capabilities` refuses a mismatch instead of
guessing a framing or a capability from another revision.

The host is the only index. Device inventory is composed live from the existing
``dir /b /s``, detailed ``dir`` and ``sd stat`` verbs - never a device-side
index, never a second framing implementation.
"""
from __future__ import annotations

import re
import time
import zlib
from typing import Dict, List, Optional

PROTO_TAG = "p4sync-1"

# Canonical capability keys the host relies on. These are also the flat keys
# the GUI sees in `status.data.caps`. Required keys are the ones the shipped
# firmware actually prints; optional keys are reported but never assumed.
CAP_KEYS = (
    "proto",             # p4sync-N
    "board",             # board profile slug
    "sd",                # mounted / absent
    "lock",              # unlocked / locked
    "magic.sdfx",        # receive/send framing magic
    "magic.pimx",        # PIMX framing magic
    "magic.bmpx",        # BMPX framing magic
    "limit.transfer",    # max bytes per receive
    "limit.rx",          # max bytes the receive pump accepts
    "limit.pim_rx",      # max bytes per PIMX receive
    "limit.inventory",   # max inventory entries
)
CAP_OPTIONAL = ("limit.screenshot", "free_bytes", "total_bytes")

# Firmware spellings -> canonical names. Keys are lower-cased, with a leading
# `sync.` removed by normalize_caps. Unknown keys stay untouched (verbatim), so
# a future firmware revision can be reported as drift without a second parser.
CAP_ALIASES = {
    "send_magic": "magic.sdfx",
    "pim_magic": "magic.pimx",
    "shot_magic": "magic.bmpx",
    # Optional in firmware; the --mock fixture advertises it so the GUI's
    # strict capability check can go green without a board.
    "shot_max": "limit.screenshot",
    "send_max": "limit.transfer",
    "rx_max": "limit.rx",
    "pim_rx_max": "limit.pim_rx",
    "inventory_max": "limit.inventory",
}

MANIFEST_VERSION = 1
# image.json fields the CLI has emitted since the prototype; `version` is
# optional for backwards compatibility with manifests already on a bench.
MANIFEST_FIELDS = ("board", "time", "pim", "files", "apps", "shot")
# BMPX proof signature. Shared by the bridge so a "PNG" path that is not a
# PNG can never be reported as a successful screenshot/full-image.
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# Stable error codes. The bridge prefixes these to its `error` string so the
# GUI can branch on them without parsing prose.
ERR_OK = "E_OK"
ERR_PROTO = "E_PROTO"          # handshake tag mismatch / absent
ERR_CAPS = "E_CAPS"           # required capability missing
ERR_TIMEOUT = "E_TIMEOUT"     # explicit device/bridge timeout
ERR_MARKER = "E_MARKER"       # sentinel/marker never arrived
ERR_CRC = "E_CRC"             # CRC verification failed
ERR_NOT_FOUND = "E_NOTFOUND"  # path/file absent
ERR_CONFLICT = "E_CONFLICT"   # both sides changed
ERR_IO = "E_IO"               # local filesystem / transport failure
ERR_CANCELLED = "E_CANCELLED"
ERR_UNSUPPORTED = "E_UNSUPPORTED"
ERROR_CODES = (
    ERR_OK, ERR_PROTO, ERR_CAPS, ERR_TIMEOUT, ERR_MARKER, ERR_CRC,
    ERR_NOT_FOUND, ERR_CONFLICT, ERR_IO, ERR_CANCELLED, ERR_UNSUPPORTED,
)

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
DIR_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2})\s+"
    r"(\d+(?:\.\d+)?\s*(?:KiB|MiB|GiB|B)|<DIR>)\s+(.+?)\s*$")
SIZE_RE = re.compile(r"^(\d+(?:\.\d+)?)\s*(B|KiB|MiB|GiB)$", re.IGNORECASE)
SIZE_UNITS = {"B": 1, "KIB": 1024, "MIB": 1024 ** 2, "GIB": 1024 ** 3}


# ---------------------------------------------------------------------------
# Handshake / capabilities
# ---------------------------------------------------------------------------

def strip_ansi(text: str) -> str:
    """Remove SGR/OSC escapes from device text (pure; no I/O)."""
    return ANSI_RE.sub("", text)


def parse_sync_status(text: str) -> Dict[str, str]:
    """Parse `sync status` into a canonical key->value dict.

    Accepts both the shipped firmware form (``sync.proto: p4sync-1``) and the
    prototype form (``proto=p4sync-1``). Lines are ``key: value`` or
    ``key=value``. Unknown keys are kept verbatim, so caller-side drift
    reporting needs no second parser.
    """
    caps: Dict[str, str] = {}
    for raw in strip_ansi(text or "").splitlines():
        line = raw.strip()
        if not line or line.startswith("P4Sync"):
            continue
        for sep in (":", "="):
            if sep in line:
                key, _, value = line.partition(sep)
                key = key.strip()
                if not key:
                    break
                # The firmware's `sync: ready (...)` success line is a status
                # sentence, not a capability; keep it but never let it stand in
                # for `sync.proto`.
                norm = normalize_key(key)
                caps[norm] = value.strip()
                if norm != key:
                    caps[key] = value.strip()
                break
    return caps


def normalize_key(key: str) -> str:
    """Canonical, lower-case name for one `sync status` key."""
    key = (key or "").strip().lower()
    if key.startswith("sync."):
        key = key[5:]
    return CAP_ALIASES.get(key, key)


def normalize_caps(caps: Dict[str, str]) -> Dict[str, str]:
    """Return caps with firmware spellings mapped to canonical names."""
    out: Dict[str, str] = {}
    for key, value in caps.items():
        out[normalize_key(key)] = str(value).strip()
    return out


def require_capabilities(caps: Dict[str, str], tag: str = PROTO_TAG) -> str:
    """Return '' when the handshake is compatible, else a human error string.

    A missing proto tag or a mismatched tag is a hard refusal: never guess a
    framing from a different firmware revision.
    """
    norm = normalize_caps(caps)
    got = norm.get("proto", "")
    if got != tag:
        return "proto mismatch: device=%r host=%r" % (got or None, tag)
    missing = [key for key in CAP_KEYS if key not in norm or not norm[key]]
    if missing:
        return "missing capabilities: %s" % ", ".join(missing)
    return ""


# ---------------------------------------------------------------------------
# Manifest (image.json)
# ---------------------------------------------------------------------------

def validate_manifest(data: object) -> str:
    """Return '' for a valid image.json object, else a human error string.

    The host manifest is the proof of a `full-image` run; a file that cannot be
    parsed must never be reported as a successful image.
    """
    if not isinstance(data, dict):
        return "manifest must be a JSON object"
    version = data.get("version", MANIFEST_VERSION)
    if version != MANIFEST_VERSION:
        return "manifest version unsupported: %r" % (version,)
    missing = [key for key in MANIFEST_FIELDS if key not in data]
    if missing:
        return "manifest missing fields: %s" % ", ".join(missing)
    if not isinstance(data.get("pim"), dict):
        return "manifest.pim must be an object"
    if not isinstance(data.get("files"), list):
        return "manifest.files must be a list"
    if not isinstance(data.get("apps"), dict):
        return "manifest.apps must be an object"
    if not str(data.get("board") or ""):
        return "manifest.board is empty"
    # The old GUI boolean-toggle bug wrote `"shot": true` into image.json.
    # A bool is not a path: rejecting it here stops a stale manifest from
    # ever being mistaken for a fresh, valid full-image proof.
    shot = data.get("shot")
    if shot is not None and not isinstance(shot, str):
        return "manifest.shot must be a PNG path string or null"
    when = data.get("time")
    if isinstance(when, bool) or not isinstance(when, (int, float)):
        return "manifest.time must be a number"
    return ""


def manifest_stale_reason(data: object) -> str:
    """Return '' when a manifest is structurally current, else why it is stale.

    Pure (no filesystem I/O): path existence and the PNG signature are checked
    by the bridge against the current mirror before it reports a rebuild.
    """
    return validate_manifest(data)


def manifest_summary(data: Dict[str, object]) -> Dict[str, object]:
    """Compact, GUI-friendly counts from an already validated manifest."""
    return {
        "board": data.get("board", ""),
        "time": data.get("time", 0),
        "pim": sorted(str(key) for key in (data.get("pim") or {}).keys()),
        "files": len(data.get("files") or []),
        "apps": sorted(str(key) for key in (data.get("apps") or {}).keys()),
        "shot": data.get("shot"),
        "version": data.get("version", MANIFEST_VERSION),
    }


# ---------------------------------------------------------------------------
# Pure parsers for existing firmware verbs (no device index, no new framing)
# ---------------------------------------------------------------------------

def parse_size(text: str) -> Optional[int]:
    """Parse the firmware's integer-only size format (`12 B`, `1.5 KiB`)."""
    match = SIZE_RE.match((text or "").strip())
    if not match:
        return None
    value = float(match.group(1))
    unit = SIZE_UNITS.get(match.group(2).upper())
    if unit is None:
        return None
    return int(value * unit)


def parse_stamp(date: str, clock: str) -> Optional[float]:
    """Parse detailed `dir` stamp (`YYYY-MM-DD`, `HH:MM`) to epoch seconds."""
    try:
        parsed = time.strptime("%s %s" % (date, clock), "%Y-%m-%d %H:%M")
        return time.mktime(parsed)
    except (ValueError, OverflowError):
        return None


def parse_dir_bare(text: str) -> List[str]:
    """Parse `dir /b /s` into normalized device paths (files and dirs)."""
    paths = []
    for raw in strip_ansi(text or "").splitlines():
        line = raw.strip().replace("\\", "/")
        if line and not line.startswith("Directory of "):
            paths.append(line)
    return paths


def parse_dir_listing(text: str, root: str = "") -> List[Dict[str, object]]:
    """Parse detailed `dir` / `dir /s` output into ``{path,size,mtime}`` rows.

    The firmware emits ``YYYY-MM-DD  HH:MM  <formatted size>  name [8.3]``.
    Under ``/s`` each level is preceded by `` Directory of <dir>``; that
    header is used to build the full path. ``<DIR>`` rows are skipped; the
    alternate 8.3 name (shown in brackets) is dropped so one file has exactly
    one identity. Sizes are the rounded, human-readable values the firmware
    prints; CRC is authoritative and is computed host-side over bytes from the
    shared sdbridge.
    """
    rows: List[Dict[str, object]] = []
    current = normalize_path(root)
    for raw in strip_ansi(text or "").splitlines():
        line = raw.strip()
        header = re.match(r"^Directory of\s+(.+)$", line, re.IGNORECASE)
        if header:
            current = normalize_path(header.group(1))
            continue
        match = DIR_RE.match(line)
        if not match or match.group(3) == "<DIR>":
            continue
        size = parse_size(match.group(3))
        if size is None:
            continue
        name = re.sub(r"\s+\[[^]]+\]\s*$", "", match.group(4)).strip()
        if not name:
            continue
        name = name.replace("\\", "/")
        path = name if name.startswith("/") else \
            ("%s/%s" % (current.rstrip("/"), name) if current else name)
        rows.append({
            "path": path,
            "size": size,
            "mtime": parse_stamp(match.group(1), match.group(2)),
        })
    return rows


def parse_sd_stat(text: str) -> Dict[str, str]:
    """Parse `sd stat <path>` into a key->value dict (`sd.path`, `sd.size`...)."""
    out: Dict[str, str] = {}
    for raw in strip_ansi(text or "").splitlines():
        line = raw.strip()
        if ":" in line:
            key, _, value = line.partition(":")
            out[key.strip()] = value.strip()
    return out


def crc32(data: bytes) -> int:
    """CRC-32 the shared SDFX trailer uses, as an unsigned 32-bit integer."""
    return zlib.crc32(data) & 0xFFFFFFFF


def file_record(path: str, data: bytes, mtime: Optional[float] = None) -> Dict[str, object]:
    """Build a host-side ``(path, size, mtime, crc32)`` inventory record."""
    return {
        "path": (path or "").replace("\\", "/"),
        "size": len(data),
        "mtime": int(mtime) if mtime is not None else 0,
        "crc32": crc32(data),
    }


def newer_wins(local: Dict[str, object], remote: Dict[str, object]) -> str:
    """Conflict policy shared with `pim_sync`: newer mtime wins, device on ties.

    Returns ``"local"`` or ``"remote"``. Equal mtimes are not a conflict: the
    device copy wins just as ``pim_sync`` lets ``rev >= local_rev`` win.
    """
    lm = int(local.get("mtime") or 0)
    rm = int(remote.get("mtime") or 0)
    return "local" if lm > rm else "remote"


def error_code(exc: BaseException) -> str:
    """Classify an exception into a stable bridge error code (pure).

    Classification is name/text based so this module stays importable without
    pyserial or a board.
    """
    text = str(exc)
    low = text.lower()
    is_p4_error = any(cls.__name__ == "P4Error" for cls in type(exc).__mro__)
    if is_p4_error or "marker" in low or "read_until" in low:
        if "marker" in low or "read_until" in low:
            return ERR_MARKER
        if "timed out" in low or "timeout" in low or "within" in low:
            return ERR_TIMEOUT
        return ERR_IO
    if "crc" in low or "mismatch" in low:
        return ERR_CRC
    if "not found" in low or "no such file" in low:
        return ERR_NOT_FOUND
    if "proto" in low or "handshake" in low:
        return ERR_PROTO
    if "caps" in low or "capabilit" in low:
        return ERR_CAPS
    return ERR_IO


def normalize_path(path: str) -> str:
    """Device paths use forward slashes; normalize a host-supplied path."""
    return (path or "").replace("\\", "/")
