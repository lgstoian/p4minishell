#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync JSON-lines bridge for the Windows GUI (host side only).

    python tools/p4sync/bridge.py [--mock] [--timeout SECONDS] [--debug]

One JSON request per stdin line, one JSON response per line on stdout:

    {"id":N,"cmd":"devices|status|push|pull|screenshot|full-image|verify|plan|shell|pkg", ...}
    {"id":N,"ok":true,"data":{...}}
    {"id":N,"ok":false,"error":"E_CODE: message"}

Events are also JSON on stdout (never prose):

    {"event":"progress","id":N,"pct":0-100,"message":"..."}
    {"event":"log","id":N,"level":"info|warn|error","message":"..."}

Human diagnostics (tracebacks in --debug) go to stderr only. All device I/O
goes through the existing P4SyncClient -> p4test Device/sdbridge stack: this
file contains no raw serial.Serial, no second framing/merge implementation and
no device-side index. ``--mock`` uses deterministic fixtures, so the GUI can be
tested with NO board attached.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import importlib
import io
import json
import os
import queue
import re
import sys
import tempfile
import threading
import time
import traceback
import types
from typing import Any, Dict, List, Optional, Tuple

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS_DIR)

from p4sync import protocol as P  # noqa: E402

# board_ports, pim_sync and P4SyncClient are imported LAZILY: they pull in
# pyserial through the shared harness, and `--mock` must work on a machine
# with no pyserial and no board at all.


class _NoSerial:
    """Import-shape placeholder, never used for I/O in mock mode."""

    def __init__(self, *args, **kwargs):
        raise RuntimeError("mock mode never opens a serial port")


def ensure_mock_imports() -> None:
    """Let the shipped harness modules import in --mock without pyserial.

    Only the *import surface* is stubbed (`serial.Serial`,
    `serial.tools.list_ports.comports`). No byte ever flows through it: mock
    I/O stays in MockClient/MockDevice below, and real mode always imports the
    genuine pyserial.
    """
    try:
        importlib.import_module("serial")  # availability probe only
        return
    except ImportError:
        # A failed import can leave a half-initialised entry behind; replace
        # it, otherwise every later `import serial` re-raises the same error.
        sys.modules.pop("serial", None)
    else:
        return
    sys.modules.pop("serial.tools", None)
    sys.modules.pop("serial.tools.list_ports", None)
    serial_mod = types.ModuleType("serial")
    serial_mod.Serial = _NoSerial
    tools_mod = types.ModuleType("serial.tools")
    list_mod = types.ModuleType("serial.tools.list_ports")
    list_mod.comports = lambda: []
    tools_mod.list_ports = list_mod
    serial_mod.tools = tools_mod
    sys.modules["serial"] = serial_mod
    sys.modules["serial.tools"] = tools_mod
    sys.modules["serial.tools.list_ports"] = list_mod

PART_SUFFIX = ".part"
MOCK_TIME = 1764162180  # fixed manifest clock for deterministic GUI fixtures
MAX_TIMEOUT = 1800.0
DEFAULT_TIMEOUTS = {
    "devices": 20.0, "status": 25.0, "shell": 30.0, "push": 90.0,
    "pull": 120.0, "screenshot": 30.0, "full-image": 600.0,
    "verify": 120.0, "plan": 180.0, "pkg": 60.0,
}
PKG_VERBS = {
    "list": "pkg list /b",
    "install": "pkg install",
    "verify": "pkg verify",
    "uninstall": "pkg remove",
    "update": "pkg outdated",
    "info": "pkg info",
    "check": "pkg check",
    "purge": "pkg purge",
    "outdated": "pkg outdated",
    "unpack": "pkg unpack",
}
PKG_NEEDS_PACKAGE = {"install", "verify", "uninstall", "info", "purge", "unpack"}
# Minimal valid 1x1 PNG (deterministic screenshot fixture for --mock).
MOCK_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
    "0000000d49444154789c636000020000050001a5f645400000000049454e44ae426082")
MOCK_STATUS = "\n".join([
    "sync.proto: p4sync-1",
    "sync.board: jc1060p470c",
    "sync.sd: mounted",
    "sync.lock: unlocked",
    "sync.send_magic: SDFX",
    "sync.pim_magic: PIMX",
    "sync.shot_magic: BMPX",
    # Optional on real firmware; advertised in --mock so the GUI's strict
    # capability list (which includes limit.screenshot) can go green.
    "sync.shot_max: 1048576",
    "sync.send_max: 1048576",
    "sync.rx_max: 2097152",
    "sync.pim_rx_max: 16777216",
    "sync.inventory_max: 512",
    "sync.free_bytes: 12451840",
    "sync: ready (p4sync-1)",
]) + "\n"
MOCK_STAMPS = {
    "/sdcard/NOTES.TXT": "2026-11-26  10:23",
    "/sdcard/notes.md": "2026-11-26  11:05",
    "/sdcard/ALARMS.ICS": "2026-11-25  09:07",
    "/sdcard/apps/DEMO/APP.BAT": "2026-11-24  18:40",
}
MOCK_FILES = {
    "/sdcard/NOTES.TXT": (b"mock notes\n", 1764162180),
    "/sdcard/notes.md": (b"# mock notes\n", 1764167100),
    "/sdcard/ALARMS.ICS": (b"BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n", 1764076020),
    "/sdcard/apps/DEMO/APP.BAT": (b"@echo off\r\necho mock demo\r\n", 1763990400),
}
MOCK_DEVICES = [
    {"port": "MOCK1", "board": "jc1060p470c", "proto": P.PROTO_TAG, "status": "online"},
    {"port": "MOCK2", "board": "m5stack_tab5", "proto": P.PROTO_TAG, "status": "online"},
]
FAILURE_WORDS = ("fail", "error", "not found", "cannot", "denied", "invalid")


class BridgeError(Exception):
    """A request failure with a stable P4Sync error code."""

    def __init__(self, code: str, message: str):
        super().__init__("%s: %s" % (code, message))
        self.code = code
        self.message = message


def _quoted(path: str) -> str:
    return '"%s"' % path if " " in path else path


def _basename(path: str) -> str:
    return P.normalize_path(path).rstrip("/").split("/")[-1]


def _parse_crc(value: Any) -> int:
    """Accept decimal, 0x-prefixed or bare 8-hex CRC strings."""
    if isinstance(value, int):
        return int(value) & 0xFFFFFFFF
    text = str(value).strip()
    try:
        return int(text, 0) & 0xFFFFFFFF
    except ValueError:
        return int(text, 16) & 0xFFFFFFFF


def artifacts_dir() -> str:
    """Deterministic, documented host directory for screenshot PNG proofs.

    ``P4SYNC_ARTIFACTS`` overrides the default ``<repo>/.agent/p4sync-artifacts``
    (git-ignored). A stable directory keeps repeated --mock captures identical
    and gives the WPF GUI a readable absolute path for preview/save-as.
    """
    configured = (os.environ.get("P4SYNC_ARTIFACTS") or "").strip()
    if configured:
        base = os.path.abspath(os.path.expanduser(configured))
    else:
        base = os.path.abspath(os.path.join(
            TOOLS_DIR, os.pardir, ".agent", "p4sync-artifacts"))
    os.makedirs(base, exist_ok=True)
    return base


def default_shot_path(port: str) -> str:
    """Stable PNG filename for one port (no wall-clock component)."""
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", str(port)).lower()
    return os.path.join(artifacts_dir(), "p4sync-shot-%s.png" % safe)


def manifest_proof_error(manifest: Dict[str, Any]) -> str:
    """Check the current mirror for a manifest's PNG proof.

    A manifest that names a missing or non-PNG shot is stale, even when its
    fields are structurally valid. Kept out of protocol.py so that module
    stays free of filesystem I/O.
    """
    shot = manifest.get("shot")
    if isinstance(shot, str) and shot:
        if not os.path.isfile(shot):
            return "manifest.shot file is missing: %s" % shot
        with open(shot, "rb") as handle:
            if not handle.read(8).startswith(P.PNG_MAGIC):
                return "manifest.shot is not a PNG: %s" % shot
    return ""


class MockSession:
    """The tiny session surface the bridge itself touches (`ser`)."""

    def __init__(self):
        self.ser = object()  # proof that full-image has a live session


class MockDevice:
    """Fake command surface; implements only existing firmware verbs."""

    def __init__(self, files: Dict[str, Tuple[bytes, int]]):
        self.files = dict(files)
        self.session = MockSession()

    def resolve(self, path: str) -> str:
        """FATFS-style case-insensitive lookup for deterministic fixtures."""
        key = P.normalize_path(path)
        if key in self.files:
            return key
        low = key.lower()
        for candidate in self.files:
            if candidate.lower() == low:
                return candidate
        base = _basename(key).lower()
        for candidate in self.files:
            if _basename(candidate).lower() == base:
                return candidate
        return key

    def run(self, cmd: str, timeout: float = 30.0, settle: float = 0.2) -> str:
        parts = cmd.strip().split()
        if not parts:
            return ""
        verb = parts[0].lower()
        arg = " ".join(parts[1:]).strip('"')
        if verb == "sync":
            return MOCK_STATUS
        if verb == "sysinfo":
            return "board.id: jc1060p470c\ndisplay: 1024 x 600\n"
        if verb == "echo" and "P4SYNC_EL=" in cmd:
            return "P4SYNC_EL=0"
        if verb == "echo":
            return cmd.strip()[5:].strip() + "\n"
        if verb == "dir":
            bare = "/b" in cmd.lower()
            root = "/sdcard"
            for token in parts[1:]:
                if not token.startswith("/") and not token.lower().startswith("/b") \
                        and not token.lower().startswith("/s"):
                    root = token.rstrip("/")
            names = sorted(p for p in self.files if p.startswith(root.rstrip("/")))
            if bare:
                return "\n".join(names) + "\n"
            grouped: Dict[str, List[str]] = {}
            for path in names:
                parent = path.rsplit("/", 1)[0] if "/" in path else ""
                grouped.setdefault(parent, []).append(path)
            lines: List[str] = []
            for parent in sorted(grouped):
                lines.append(" Directory of %s\n" % (parent or root))
                for path in sorted(grouped[parent]):
                    data, _mtime = self.files[path]
                    stamp = MOCK_STAMPS.get(path, "2026-01-01  00:00")
                    lines.append("%s  %10d B  %s"
                                 % (stamp, len(data), _basename(path)))
            lines.append("%15d file(s)\n" % len(names))
            return "\n".join(lines)
        if verb == "sd" and len(parts) >= 3 and parts[1].lower() == "stat":
            path = self.resolve(parts[2].strip('"'))
            if path not in self.files:
                return "sd: path not found %s\n" % path
            data, _mtime = self.files[path]
            return ("sd.path: %s\nsd.type: file\nsd.size: %d B\nsd.mode: 0644\n"
                    % (path, len(data)))
        if verb == "attrib":
            return "A %s\n" % arg
        if verb in ("ren", "rename"):
            names = cmd.strip().split()
            if len(names) >= 3:
                old, new = names[1].strip('"'), names[2].strip('"')
                if old in self.files:
                    # Firmware `ren` replaces only the final component; the
                    # file stays in the same directory.
                    parent = old.rsplit("/", 1)[0] if "/" in old else ""
                    target = ("%s/%s" % (parent, new)) if parent else new
                    self.files[target] = self.files.pop(old)
                    return "Renamed %s -> %s\n" % (old, target)
            return "rename: not found\n"
        if verb == "pkg":
            if "list" in parts[1:2]:
                return "DEMO|Mock demo|1.0|app|3|v1\n"
            return "pkg %s ok\n" % (" ".join(parts[1:]))
        if verb == "apps":
            return "DEMO  Mock demo\n"
        if verb == "launch" and "/list" in cmd:
            return "DEMO  Mock demo\n"
        return ""


class MockShot:
    """The p4test screenshot surface (`width`, `height`, `save_png`)."""

    width = 1024
    height = 600

    def save_png(self, out_path: str) -> None:
        with open(out_path, "wb") as handle:
            handle.write(MOCK_PNG)


class MockClient:
    """Deterministic stand-in for P4SyncClient; no serial is ever opened."""

    def __init__(self, port: str = "MOCK1"):
        self.port = port
        self.dev = MockDevice(MOCK_FILES)
        self._caps = P.parse_sync_status(MOCK_STATUS)

    def close(self) -> None:
        return None

    def status(self) -> str:
        return MOCK_STATUS

    def check_status(self) -> Tuple[bool, str]:
        return (P.PROTO_TAG in MOCK_STATUS, MOCK_STATUS)

    def board_id(self) -> str:
        return "jc1060p470c"

    def display_size(self) -> Tuple[int, int]:
        return (1024, 600)

    def push_file(self, remote: str, data: bytes) -> None:
        self.dev.files[self.dev.resolve(remote)] = (bytes(data), int(time.time()))

    def push_local(self, local: str, remote: Optional[str] = None) -> None:
        with open(local, "rb") as handle:
            self.push_file(remote or os.path.basename(local), handle.read())

    def pull_file(self, remote: str) -> bytes:
        key = self.dev.resolve(remote)
        if key not in self.dev.files:
            raise BridgeError(P.ERR_NOT_FOUND, "mock file not found: %s" % key)
        return self.dev.files[key][0]

    def screenshot(self, out_path: Optional[str] = None):
        if out_path:
            MockShot().save_png(out_path)
        return MockShot()


class Bridge:
    """One JSON-lines request dispatcher. Thread-safe stdout emission only."""

    def __init__(self, mock: bool = False, timeout: Optional[float] = None,
                 debug: bool = False):
        self.mock = bool(mock)
        self.timeout = timeout
        self.debug = debug
        if self.mock:
            ensure_mock_imports()
        self._emit_lock = threading.Lock()
        self._cancel = threading.Event()
        self._clients: Dict[str, Any] = {}

    # -- JSON framing ------------------------------------------------------
    def emit(self, obj: Dict[str, Any]) -> None:
        line = json.dumps(obj, ensure_ascii=False, default=str)
        with self._emit_lock:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()

    def progress(self, ident: Any, pct: int, message: str) -> None:
        if getattr(self, "human", False):
            # The CLI keeps stdout clean: one JSON object with --json, one
            # human summary otherwise. Progress belongs on stderr there.
            self.stderr("progress %d%%: %s" % (pct, message))
            return
        self.emit({"event": "progress", "id": ident,
                   "pct": max(0, min(100, int(pct))), "message": message})

    def log(self, ident: Any, level: str, message: str) -> None:
        if getattr(self, "human", False):
            self.stderr("[%s] %s" % (level, message))
            return
        self.emit({"event": "log", "id": ident, "level": level,
                   "message": message})

    def stderr(self, message: str) -> None:
        sys.stderr.write(message.rstrip() + "\n")
        sys.stderr.flush()

    def ok(self, ident: Any, data: Dict[str, Any]) -> None:
        self.emit({"id": ident, "ok": True, "data": data})

    def fail(self, ident: Any, error: BridgeError) -> None:
        self.emit({"id": ident, "ok": False, "error": str(error)})

    def cancel(self) -> None:
        self._cancel.set()

    def check_cancel(self, ident: Any) -> None:
        if self._cancel.is_set():
            raise BridgeError(P.ERR_CANCELLED, "operation cancelled by client")

    # -- helpers -----------------------------------------------------------
    @staticmethod
    def arg(req: Dict[str, Any], name: str) -> str:
        value = req.get(name)
        if value is None or (isinstance(value, str) and not value.strip()):
            raise BridgeError(P.ERR_IO, "missing argument: %s" % name)
        return str(value)

    @staticmethod
    def number(req: Dict[str, Any], name: str, default: float,
               minimum: float = 0.0, maximum: float = MAX_TIMEOUT) -> float:
        value = req.get(name, default)
        try:
            number = float(value)
        except (TypeError, ValueError):
            raise BridgeError(P.ERR_IO, "%s must be a number" % name)
        return max(minimum, min(maximum, number))

    def timeout_for(self, req: Dict[str, Any], cmd: str) -> float:
        default = self.timeout if self.timeout is not None \
            else DEFAULT_TIMEOUTS.get(cmd, 60.0)
        return self.number(req, "timeout", default, 0.5)

    def port_for(self, req: Dict[str, Any]) -> str:
        port = req.get("port") or os.environ.get("P4SYNC_PORT") or \
            ("MOCK1" if self.mock else "")
        if not port:
            raise BridgeError(P.ERR_IO,
                              "missing argument: port (or set P4SYNC_PORT)")
        return str(port).upper()

    def client_for(self, port: str, req: Dict[str, Any]) -> Any:
        port = str(port).upper()
        client = self._clients.get(port)
        if client is not None:
            return client
        try:
            if self.mock:
                client = MockClient(port)
            else:
                from p4sync.client import P4SyncClient  # lazy: needs pyserial
                client = P4SyncClient(port, quick=bool(req.get("quick")))
        except Exception as exc:  # noqa: BLE001 - reported as a bridge error
            code = P.error_code(exc)
            raise BridgeError(code, "cannot open %s: %s" % (port, exc))
        self._clients[port] = client
        return client

    def drop_client(self, port: str) -> None:
        client = self._clients.pop(str(port).upper(), None)
        if client is not None:
            try:
                client.close()
            except Exception:  # noqa: BLE001
                pass

    def handshake(self, client: Any, req: Dict[str, Any],
                  fresh: bool = False) -> Dict[str, str]:
        """Prove the proto tag once per session; never guess a framing."""
        caps = getattr(client, "_caps", None)
        if fresh or not caps:
            raw = client.status()
            caps = P.parse_sync_status(raw)
            error = P.require_capabilities(caps)
            if error:
                code = P.ERR_PROTO if "proto" in error else P.ERR_CAPS
                raise BridgeError(code, error)
            client._caps = caps
        return caps

    @staticmethod
    def _check_output(output: str, what: str) -> None:
        low = output.lower()
        if any(word in low for word in FAILURE_WORDS):
            raise BridgeError(P.ERR_IO, "%s: %s" % (what, output.strip()[-200:]))

    def run_command(self, client: Any, command: str, timeout: float,
                    settle: float = 0.2, errorlevel: bool = True) -> Dict[str, Any]:
        """Execute ONE existing firmware verb via marker-synced Device.run."""
        output = P.strip_ansi(client.dev.run(command, timeout=timeout,
                                             settle=settle))
        level = None
        if errorlevel:
            probe = client.dev.run("echo P4SYNC_EL=%ERRORLEVEL%",
                                   timeout=min(timeout, 30.0), settle=settle)
            match = re.search(r"P4SYNC_EL=(-?\d+)", probe)
            if match:
                level = int(match.group(1))
        return {"command": command, "output": output, "errorlevel": level}

    # -- one-file transfer helpers (existing sdbridge, no parallel framing) -
    def push_one(self, client: Any, local: str, remote: str,
                 ident: Any, timeout: float) -> Dict[str, Any]:
        self.check_cancel(ident)
        if not os.path.isfile(local):
            raise BridgeError(P.ERR_NOT_FOUND, "local file not found: %s" % local)
        with open(local, "rb") as handle:
            data = handle.read()
        st = os.stat(local)
        record = P.file_record(remote, data, st.st_mtime)
        remote = P.normalize_path(remote)
        part = remote + PART_SUFFIX
        self.progress(ident, 0, "push %s (%d bytes)" % (remote, len(data)))
        # The device's receive path already verifies the CRC trailer; we still
        # read the .part back through sdbridge and verify before the atomic
        # rename, so a partial/lost transfer can never become the final file.
        client.push_file(part, data)
        back = client.pull_file(part)
        if len(back) != len(data) or P.crc32(back) != record["crc32"]:
            raise BridgeError(P.ERR_CRC,
                              ".part CRC/size mismatch for %s" % remote)
        self._check_output(
            client.dev.run("ren %s %s" % (_quoted(part), _quoted(_basename(remote))),
                           timeout=min(timeout, 30.0)), "rename %s" % remote)
        self.progress(ident, 100, "pushed %s" % remote)
        record["part_verified"] = True
        return record

    def pull_one(self, client: Any, remote: str, local: str, ident: Any,
                 timeout: float, expected: Optional[int] = None) -> Dict[str, Any]:
        self.check_cancel(ident)
        remote = P.normalize_path(remote)
        self.progress(ident, 0, "pull %s" % remote)
        data = client.pull_file(remote)
        actual = P.crc32(data)
        if expected is not None and actual != int(expected):
            raise BridgeError(P.ERR_CRC,
                              "CRC mismatch for %s: got %08x want %08x"
                              % (remote, actual, int(expected)))
        parent = os.path.dirname(os.path.abspath(local))
        os.makedirs(parent, exist_ok=True)
        temp_path = None
        try:
            with tempfile.NamedTemporaryFile(
                    mode="wb", prefix=".p4sync-", suffix=PART_SUFFIX,
                    dir=parent, delete=False) as handle:
                temp_path = handle.name
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            # Verify what actually landed on disk, then rename atomically.
            with open(temp_path, "rb") as handle:
                saved = handle.read()
            if len(saved) != len(data) or P.crc32(saved) != actual:
                raise BridgeError(P.ERR_CRC, "local .part verification failed")
            os.replace(temp_path, local)
            temp_path = None
        finally:
            if temp_path is not None and os.path.exists(temp_path):
                try:
                    os.unlink(temp_path)
                except OSError:
                    pass
        self.progress(ident, 100, "pulled %s -> %s" % (remote, local))
        return P.file_record(local, data, os.stat(local).st_mtime)

    # -- commands ----------------------------------------------------------
    def cmd_devices(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        if self.mock:
            return {"devices": [dict(item) for item in MOCK_DEVICES]}
        import board_ports  # lazy: needs pyserial
        mapping = board_ports._load_map()
        usb_to_board = {usb: slug for slug, usb in mapping.items() if usb}
        devices = []
        for port in board_ports._candidate_ports():
            info = board_ports._port_info(port)
            usb = (info.serial_number if info else None) or ""
            board = usb_to_board.get(usb, "")
            state = "mapped" if board else "present"
            if req.get("probe"):
                probed = board_ports._probe_board_id(port)
                if probed:
                    board = probed
                    state = "online"
            devices.append({"port": port, "board": board, "proto": "",
                            "status": state})
        return {"devices": devices}

    def cmd_status(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        client = self.client_for(port, req)
        caps = self.handshake(client, req, fresh=True)
        width, height = client.display_size()
        data = {
            "port": port, "board": caps.get("board") or client.board_id(),
            "proto": caps.get("proto", ""), "sd": caps.get("sd", ""),
            "lock": caps.get("lock", ""), "caps": caps,
            "display": [width, height],
        }
        if "free_bytes" in caps:
            data["free_bytes"] = caps["free_bytes"]
        return data

    def cmd_shell(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        command = self.arg(req, "command")
        timeout = self.timeout_for(req, "shell")
        settle = self.number(req, "settle", 0.2, 0.0, 30.0)
        plain = bool(req.get("plain", False))
        client = self.client_for(port, req)
        self.handshake(client, req)
        result = self.run_command(client, command, timeout, settle,
                                  errorlevel=not plain)
        result.update({"port": port, "ok": result["errorlevel"] in (0, None)})
        return result

    def cmd_push(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        local = self.arg(req, "local")
        remote = self.arg(req, "remote")
        timeout = self.timeout_for(req, "push")
        client = self.client_for(port, req)
        self.handshake(client, req)
        record = self.push_one(client, local, remote, ident, timeout)
        return {"port": port, "local": local, "remote": P.normalize_path(remote),
                "bytes": record["size"], "crc32": "%08x" % record["crc32"],
                "part_verified": True}

    def cmd_pull(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        remote = self.arg(req, "remote")
        local = self.arg(req, "local")
        timeout = self.timeout_for(req, "pull")
        expected = req.get("crc32")
        if expected is None:
            expected = req.get("expected_crc32")
        client = self.client_for(port, req)
        self.handshake(client, req)
        wanted = _parse_crc(expected) if expected is not None else None
        record = self.pull_one(client, remote, local, ident, timeout, wanted)
        return {"port": port, "remote": P.normalize_path(remote),
                "local": os.path.abspath(local), "path": os.path.abspath(local),
                "bytes": record["size"], "crc32": "%08x" % record["crc32"],
                "part_verified": True}

    def cmd_verify(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        remote = self.arg(req, "remote")
        expected = req.get("crc32")
        if expected is None:
            raise BridgeError(P.ERR_IO, "missing argument: crc32")
        timeout = self.timeout_for(req, "verify")
        client = self.client_for(port, req)
        self.handshake(client, req)
        data = client.pull_file(P.normalize_path(remote))
        actual = P.crc32(data)
        want = _parse_crc(expected)
        return {"port": port, "remote": P.normalize_path(remote),
                "ok": actual == want, "crc32": "%08x" % actual,
                "expected": "%08x" % want, "bytes": len(data)}

    def cmd_screenshot(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        timeout = self.timeout_for(req, "screenshot")
        client = self.client_for(port, req)
        self.handshake(client, req)
        out = req.get("out") or req.get("path") or req.get("png")
        if not out:
            out = default_shot_path(port)
        out = os.path.abspath(out)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        self.progress(ident, 0, "capturing screen")
        # The ONE BMPX capture, through P4SyncClient -> p4test.screenshot.
        shot = client.screenshot(out)
        self.progress(ident, 100, "screen captured")
        # Read back what that capture wrote. No second capture and no invented
        # payload: a missing/non-PNG file is a hard error, never a false green.
        if not os.path.isfile(out):
            raise BridgeError(P.ERR_IO, "screenshot did not write a PNG: %s" % out)
        with open(out, "rb") as handle:
            png = handle.read()
        if not png.startswith(P.PNG_MAGIC):
            raise BridgeError(P.ERR_IO, "screenshot is not a PNG: %s" % out)
        return {"port": port, "out": out, "path": out, "png": out,
                "png_base64": base64.b64encode(png).decode("ascii"),
                "sha256": hashlib.sha256(png).hexdigest(),
                "size": len(png), "bytes": len(png),
                "width": shot.width, "height": shot.height}

    @staticmethod
    def stale_manifest_reason(manifest_path: str) -> str:
        """Why an existing image.json is stale/unusable; '' when none.

        Reuses the shared structural validator, then checks the current mirror:
        the PNG proof a manifest names must still exist and really be a PNG.
        """
        if not os.path.isfile(manifest_path):
            return ""
        try:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                manifest = json.load(handle)
        except (OSError, ValueError) as exc:
            return "unreadable image.json: %s" % exc
        reason = P.manifest_stale_reason(manifest)
        return reason or manifest_proof_error(manifest)

    def cmd_full_image(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        mirror = self.arg(req, "mirror")
        db = self.arg(req, "db")
        timeout = self.timeout_for(req, "full-image")
        client = self.client_for(port, req)
        self.handshake(client, req)
        # Imported here, not at module scope: the CLI imports Bridge, so a
        # top-level import here would create a circular import.
        from p4sync.__main__ import cmd_full_image  # noqa: E402

        # The GUI sends `files`/`shot` as booleans (feature toggles); the
        # shipped CLI expects paths. Coerce before calling it: passing True
        # would make os.walk()/open() treat it as a file descriptor (and could
        # write PNG bytes into stdout, breaking JSON framing).
        files = req.get("files")
        if isinstance(files, bool):
            files = os.path.join(mirror, "files") if files else None
        elif files is not None and not isinstance(files, str):
            raise BridgeError(P.ERR_IO, "files must be a path or a boolean")
        shot = req.get("shot")
        if isinstance(shot, bool):
            shot = os.path.join(mirror, "shot.png") if shot else None
        elif shot is not None and not isinstance(shot, str):
            raise BridgeError(P.ERR_IO, "shot must be a path or a boolean")
        args = argparse.Namespace(mirror=mirror, db=db,
                                  files=files, shot=shot)
        os.makedirs(mirror, exist_ok=True)
        manifest_path = os.path.join(mirror, "image.json")
        # A manifest left by the old boolean-toggle bug (or a half-finished
        # earlier run) must never be mistaken for this run's proof. Drop it
        # before rebuilding and record why on the log channel.
        stale_reason = self.stale_manifest_reason(manifest_path)
        if stale_reason:
            os.unlink(manifest_path)
            self.log(ident, "warn",
                     "stale image.json (%s) removed; rebuilt from current mirror"
                     % stale_reason)
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            if self.mock:
                # Only the PIM engine is stubbed; every other stage (handshake,
                # file provisioning, inventory, screenshot, manifest) runs the
                # shipped CLI code against deterministic fixtures.
                def fake_sync(_ser, kind, name, path):
                    os.makedirs(path, exist_ok=True)
                    ext = ".ics" if kind == "ics" else ".vcf"
                    sample = os.path.join(path, "MOCK" + ext)
                    body = (b"BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n" if ext == ".ics"
                            else b"BEGIN:VCARD\r\nEND:VCARD\r\n")
                    with open(sample, "wb") as handle:
                        handle.write(body)
                    print("SYNC ok (mock) %s" % name)
                    return 0

                # A module-level stub keeps the mock path independent of
                # shell_session/pyserial (and of any platform socket setup).
                # The real PIM engine is untouched: real mode imports it via
                # the shipped CLI function above.
                import unittest.mock as mocklib
                pim_stub = types.ModuleType("pim_sync")
                pim_stub.cmd_sync = fake_sync
                with mocklib.patch.dict(sys.modules, {"pim_sync": pim_stub}):
                    rc = cmd_full_image(client, args)
            else:
                rc = cmd_full_image(client, args)
        if not os.path.isfile(manifest_path):
            raise BridgeError(P.ERR_IO,
                              "full-image did not write a manifest: %s" % manifest_path)
        with open(manifest_path, "r", encoding="utf-8") as handle:
            try:
                manifest = json.load(handle)
            except ValueError as exc:
                raise BridgeError(P.ERR_IO, "unreadable image.json: %s" % exc)
        if self.mock:
            # The shipped CLI stamps wall-clock time; pin it for fixtures so
            # the GUI sees byte-identical manifest data on every --mock run.
            manifest["time"] = MOCK_TIME
            with open(manifest_path, "w", encoding="utf-8") as handle:
                json.dump(manifest, handle, indent=2)
        # Re-validate what this run actually wrote, including the PNG proof in
        # the current mirror. A stale/invalid manifest is a hard failure, never
        # a green response built on a file from an earlier run.
        error = P.manifest_stale_reason(manifest) or manifest_proof_error(manifest)
        if error:
            raise BridgeError(P.ERR_IO, "invalid image.json: %s" % error)
        manifest["version"] = P.MANIFEST_VERSION
        return {"port": port, "manifest": manifest,
                "manifest_path": manifest_path,
                "failures": int(rc != 0), "ok": rc == 0,
                "stale_manifest": bool(stale_reason),
                "rebuilt": bool(stale_reason),
                "stale_reason": stale_reason,
                "summary": P.manifest_summary(manifest),
                "output": captured.getvalue()}

    def cmd_pkg(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        verb = self.arg(req, "verb").lower()
        package = req.get("package")
        if verb not in PKG_VERBS:
            raise BridgeError(P.ERR_UNSUPPORTED, "unsupported pkg verb: %s" % verb)
        if verb in PKG_NEEDS_PACKAGE and not package:
            raise BridgeError(P.ERR_IO, "missing argument: package")
        template = PKG_VERBS[verb]
        command = template if package is None else "%s %s" % (
            template, _quoted(str(package)))
        timeout = self.timeout_for(req, "pkg")
        client = self.client_for(port, req)
        self.handshake(client, req)
        result = self.run_command(client, command, timeout)
        rows = []
        for raw in result["output"].splitlines():
            line = raw.strip()
            if line and "|" in line:
                rows.append(dict(zip(
                    ("app", "title", "version", "type", "files", "tag"),
                    line.split("|"))))
        result.update({"port": port, "verb": verb,
                       "package": str(package) if package else "",
                       "rows": rows,
                       "ok": result["errorlevel"] in (0, None)})
        return result

    def cmd_plan(self, req: Dict[str, Any], ident: Any) -> Dict[str, Any]:
        port = self.port_for(req)
        local_root = self.arg(req, "local")
        remote_root = P.normalize_path(req.get("remote") or "/sdcard")
        timeout = self.timeout_for(req, "plan")
        dry_run = bool(req.get("dry_run", True))
        apply_actions = bool(req.get("apply", False)) and not dry_run
        if not os.path.isdir(local_root):
            raise BridgeError(P.ERR_NOT_FOUND,
                              "local directory not found: %s" % local_root)
        client = self.client_for(port, req)
        self.handshake(client, req)

        # Host inventory: the only index lives here (size/mtime/crc32).
        local: Dict[str, Dict[str, Any]] = {}
        for root, _dirs, names in os.walk(local_root):
            for name in sorted(names):
                if name.endswith(PART_SUFFIX):
                    continue
                path = os.path.join(root, name)
                with open(path, "rb") as handle:
                    data = handle.read()
                rel = os.path.relpath(path, local_root).replace(os.sep, "/")
                local[rel] = P.file_record(rel, data, os.stat(path).st_mtime)

        # Device inventory is composed live from the EXISTING verbs.
        bare = client.dev.run("dir /b /s %s" % _quoted(remote_root),
                              timeout=timeout)
        detailed = client.dev.run("dir /s %s" % _quoted(remote_root),
                                  timeout=timeout)
        meta = {row["path"]: row
                for row in P.parse_dir_listing(detailed, root=remote_root)}
        remote: Dict[str, Dict[str, Any]] = {}
        paths = P.parse_dir_bare(bare)
        self.progress(ident, 10, "device inventory: %d path(s)" % len(paths))
        for index, path in enumerate(paths):
            self.check_cancel(ident)
            if not meta.get(path):
                stat_text = client.dev.run("sd stat %s" % _quoted(path),
                                           timeout=min(timeout, 30.0))
                stat = P.parse_sd_stat(stat_text)
                if not stat.get("sd.type") or "not found" in stat_text.lower():
                    continue  # directory or a vanished entry: no file to sync
                size = P.parse_size(stat.get("sd.size", ""))
                if size is None:
                    continue
                meta[path] = {"path": path, "size": size, "mtime": None}
            row = meta[path]
            rel = path[len(remote_root.rstrip("/")) + 1:] \
                if path.startswith(remote_root.rstrip("/") + "/") else \
                _basename(path)
            record = {"path": rel, "device_path": path,
                      "size": int(row["size"] or 0),
                      "mtime": int(row["mtime"] or 0), "crc32": 0}
            remote[rel] = record
            if rel in local and local[rel]["size"] == record["size"]:
                # Authoritative CRC only for plausible candidates; never trust
                # the rounded size/date printed by `dir` for a skip decision.
                pulled = client.pull_file(path)
                record["crc32"] = P.crc32(pulled)
            if len(paths) > 1:
                self.progress(ident, 10 + int(50 * (index + 1) / len(paths)),
                              "device inventory %d/%d" % (index + 1, len(paths)))

        cache_path = req.get("cache")
        cached: Dict[str, Dict[str, Any]] = {}
        if cache_path and os.path.isfile(str(cache_path)):
            try:
                with open(str(cache_path), "r", encoding="utf-8") as handle:
                    cached = json.load(handle)
            except (OSError, ValueError):
                cached = {}

        actions: List[Dict[str, Any]] = []
        for rel in sorted(set(local) | set(remote)):
            tgt = "%s/%s" % (remote_root.rstrip("/"), rel)
            src = os.path.join(local_root, rel.replace("/", os.sep))
            item = local.get(rel)
            other = remote.get(rel)
            if item is None:
                actions.append({"op": "pull", "path": rel,
                                "reason": "remote-only", "remote": tgt,
                                "local": src.replace(os.sep, "/"),
                                "size": other["size"]})
                continue
            if other is None:
                actions.append({"op": "push", "path": rel,
                                "reason": "local-only", "remote": tgt,
                                "local": src.replace(os.sep, "/"),
                                "size": item["size"],
                                "crc32": "%08x" % item["crc32"]})
                continue
            same = (item["size"] == other["size"] and
                    (not other["crc32"] or item["crc32"] == other["crc32"]))
            if same:
                actions.append({"op": "skip", "path": rel,
                                "reason": "identical", "size": item["size"]})
                continue
            winner = P.newer_wins(item, other)
            old = cached.get(rel)
            both_changed = bool(old and old.get("crc32") != item["crc32"] and
                                (old.get("size") != other["size"] or
                                 old.get("mtime") != other["mtime"]))
            actions.append({
                "op": "push" if winner == "local" else "pull",
                "path": rel,
                "reason": "newer-local" if winner == "local" else "newer-remote",
                "remote": tgt, "local": src.replace(os.sep, "/"),
                "size": item["size"] if winner == "local" else other["size"],
                "conflict": both_changed,
                "crc32": "%08x" % (item["crc32"] if winner == "local"
                                   else other["crc32"]),
            })

        summary = {
            "files": len(actions),
            "push": sum(1 for a in actions if a["op"] == "push"),
            "pull": sum(1 for a in actions if a["op"] == "pull"),
            "skip": sum(1 for a in actions if a["op"] == "skip"),
            "conflicts": sum(1 for a in actions if a.get("conflict")),
            "bytes_push": sum(int(a.get("size") or 0) for a in actions
                              if a["op"] == "push"),
            "bytes_pull": sum(int(a.get("size") or 0) for a in actions
                              if a["op"] == "pull"),
            "dry_run": dry_run,
        }
        applied = 0
        if apply_actions:
            for index, action in enumerate(actions):
                self.check_cancel(ident)
                if action["op"] == "push":
                    self.push_one(client, action["local"], action["remote"],
                                  ident, timeout)
                elif action["op"] == "pull":
                    self.pull_one(client, action["remote"], action["local"],
                                  ident, timeout)
                else:
                    continue  # `skip` is not a transfer; never count it as one
                applied += 1
                self.progress(ident, 60 + int(40 * (index + 1) / max(1, len(actions))),
                              "%s %s (%d/%d)" % (action["op"], action["path"],
                                                 index + 1, len(actions)))
        elif cache_path and not dry_run:
            with open(str(cache_path), "w", encoding="utf-8") as handle:
                json.dump(local, handle, indent=2, sort_keys=True)
        summary["applied"] = applied
        return {"actions": actions, "summary": summary,
                "local_root": local_root, "remote_root": remote_root,
                "dry_run": dry_run}

    # -- dispatch ----------------------------------------------------------
    COMMANDS = {
        "devices": cmd_devices, "status": cmd_status, "shell": cmd_shell,
        "push": cmd_push, "pull": cmd_pull, "screenshot": cmd_screenshot,
        "full-image": cmd_full_image, "verify": cmd_verify, "plan": cmd_plan,
        "pkg": cmd_pkg,
    }

    def handle(self, raw: str) -> None:
        ident: Any = None
        try:
            req = json.loads(raw)
            if not isinstance(req, dict):
                raise BridgeError(P.ERR_IO, "request must be a JSON object")
            ident = req.get("id")
            cmd = req.get("cmd")
            if not isinstance(cmd, str) or cmd not in self.COMMANDS:
                raise BridgeError(P.ERR_UNSUPPORTED,
                                  "unknown cmd: %s" % (cmd,))
            handler = self.COMMANDS[cmd]
            data = handler(self, req, ident)
            self.ok(ident, data)
        except BridgeError as exc:
            self.fail(ident, exc)
            self.stderr("%s" % exc)
        except Exception as exc:  # noqa: BLE001 - one bad request must not kill the bridge
            code = P.error_code(exc)
            self.fail(ident, BridgeError(code, str(exc)))
            self.stderr("%s: %s" % (code, exc))
            if self.debug:
                traceback.print_exc(file=sys.stderr)

    def close(self) -> None:
        for port in list(self._clients):
            self.drop_client(port)


def _stdin_reader(inbox: "queue.Queue[Optional[str]]", bridge: Bridge) -> None:
    """Read stdin in a thread so `cancel` can arrive during a long transfer."""
    try:
        for line in sys.stdin:
            if not line.strip():
                continue
            try:
                req = json.loads(line)
            except ValueError:
                inbox.put(line)
                continue
            if isinstance(req, dict) and req.get("cmd") == "cancel":
                bridge.cancel()
                bridge.ok(req.get("id"), {"cancelled": True})
                continue
            inbox.put(line)
    except (OSError, ValueError):
        pass
    finally:
        inbox.put(None)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mock", action="store_true",
                        help="deterministic fixtures; no board and no serial")
    parser.add_argument("--timeout", type=float, default=None,
                        help="default device timeout in seconds (per request overrides)")
    parser.add_argument("--debug", action="store_true",
                        help="print tracebacks to stderr only")
    args = parser.parse_args(argv)

    bridge = Bridge(mock=args.mock, timeout=args.timeout, debug=args.debug)
    inbox: "queue.Queue[Optional[str]]" = queue.Queue()
    reader = threading.Thread(target=_stdin_reader, args=(inbox, bridge),
                              daemon=True)
    reader.start()
    try:
        while True:
            line = inbox.get()
            if line is None:
                break
            bridge._cancel.clear()
            bridge.handle(line)
    except KeyboardInterrupt:
        bridge.stderr("bridge interrupted")
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
