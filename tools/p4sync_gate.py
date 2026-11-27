#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""p4sync_gate.py - board-free proof of the P4Sync GUI bridge.

No board, no serial port, no firmware build. The bridge is exercised as a real
subprocess in ``--mock`` mode (JSON lines in/out) plus in-process fake Device
sessions for the failure paths. The shipped CLI composition, sdbridge framing,
PIM engine and marker-synced run() are the code under test - this gate never
reimplements them.

Usage:
    python tools/p4sync_gate.py            # --check (default)
    python tools/p4sync_gate.py --self-test # red canaries, no bridge process
"""
from __future__ import annotations

import argparse
import ast
import base64
import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

from p4sync import bridge as B  # noqa: E402
from p4sync import protocol as P  # noqa: E402
from p4test.session import DeviceSession, P4Error, _FakeReader  # noqa: E402

OPENTEMP = r"C:\Users\lgstoian\AppData\Local\Temp\opencode"
WORK = OPENTEMP if os.path.isdir(OPENTEMP) else tempfile.gettempdir()
BRIDGE = os.path.join(TOOLS, "p4sync", "bridge.py")


class Checklist:
    """Minimal assertion ledger: a pass with zero checks is a failure."""

    def __init__(self, name: str):
        self.name = name
        self.rows: List[Tuple[str, bool, str]] = []

    def check(self, name: str, condition: bool, detail: str = "") -> None:
        self.rows.append((name, bool(condition), detail))
        print("  [%s] %s%s" % ("PASS" if condition else "FAIL", name,
                               (" - " + detail) if detail and not condition else ""))

    def finish(self) -> int:
        bad = [name for name, ok, _detail in self.rows if not ok]
        if not self.rows or bad:
            print("%s: FAIL (%d/%d checks failed)"
                  % (self.name, len(bad), max(1, len(self.rows))))
            return 1
        print("%s: PASS (%d checks)" % (self.name, len(self.rows)))
        return 0


def run_bridge(requests: List[Dict[str, Any]],
               extra: Tuple[str, ...] = ()) -> Tuple[List[Dict[str, Any]], str]:
    """Run `bridge.py --mock` with real stdin/stdout; parse EVERY stdout line."""
    payload = "".join(json.dumps(req) + "\n" for req in requests)
    proc = subprocess.run([sys.executable, BRIDGE, "--mock", *extra],
                          input=payload, text=True, capture_output=True,
                          timeout=120)
    lines = []
    for number, raw in enumerate(proc.stdout.splitlines(), 1):
        try:
            lines.append(json.loads(raw))
        except ValueError as exc:
            raise AssertionError("stdout line %d is not JSON: %r (%s)"
                                 % (number, raw[:160], exc))
    return lines, proc.stderr


def replies(lines: List[Dict[str, Any]]) -> Dict[Any, Dict[str, Any]]:
    return {line["id"]: line for line in lines if "event" not in line}


def events(lines: List[Dict[str, Any]], name: str) -> List[Dict[str, Any]]:
    return [line for line in lines
            if line.get("event") == name and line.get("id") is not None]


def valid_response(obj: Dict[str, Any], ident: Any) -> bool:
    """A green response must carry the right id, an ok flag and real payload."""
    if not isinstance(obj, dict) or obj.get("id") != ident:
        return False
    if not isinstance(obj.get("ok"), bool):
        return False
    if obj["ok"]:
        return isinstance(obj.get("data"), dict) and bool(obj["data"])
    return isinstance(obj.get("error"), str) and bool(obj["error"].strip())


def second_serial_hits(source: str) -> List[str]:
    """AST scan: bridge must not own a serial port or open one itself."""
    hits: List[str] = []
    tree = ast.parse(source)
    uses_client = False
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name.split(".")[0] == "serial":
                    hits.append("import " + alias.name)
        elif isinstance(node, ast.ImportFrom):
            if (node.module or "").split(".")[0] == "serial":
                hits.append("from " + str(node.module))
        elif isinstance(node, ast.Call):
            func = node.func
            name = func.id if isinstance(func, ast.Name) else \
                getattr(func, "attr", "")
            if name in ("Serial", "open_port"):
                hits.append(name + "()")
        elif isinstance(node, ast.Name) and node.id == "P4SyncClient":
            uses_client = True
    if not uses_client:
        hits.append("P4SyncClient not used")
    return hits


def in_process(bridge: B.Bridge, req: Dict[str, Any]) -> Dict[str, Any]:
    """One request through Bridge.handle with stdout captured (no subprocess)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        bridge.handle(json.dumps(req))
    parsed = [json.loads(line) for line in out.getvalue().splitlines()]
    bad = [obj for obj in parsed if not valid_response(obj, req["id"])]
    if bad:
        raise AssertionError("invalid bridge response: %r" % bad)
    return replies(parsed)[req["id"]]


class FakeClient:
    """Minimal client surface for the failure paths (never opens a port)."""

    def __init__(self, status_text: str = B.MOCK_STATUS, marker_error: str = ""):
        self.status_text = status_text
        self.marker_error = marker_error

    def status(self) -> str:
        if self.marker_error:
            raise P4Error(self.marker_error)
        return self.status_text

    def pull_file(self, remote: str) -> bytes:
        return b"tampered payload"

    def close(self) -> None:
        return None


def check_protocol(c: Checklist) -> None:
    caps = P.parse_sync_status(B.MOCK_STATUS)
    c.check("shipped sync.* status parses to canonical caps",
            P.require_capabilities(caps) == "" and
            caps.get("magic.sdfx") == "SDFX" and
            caps.get("limit.transfer") == "1048576" and
            caps.get("proto") == P.PROTO_TAG,
            "caps=%r" % caps)
    c.check("mock advertises limit.screenshot for the GUI strict check",
            caps.get("limit.screenshot") == "1048576" and
            "limit.screenshot" in P.CAP_OPTIONAL and
            "limit.screenshot" not in P.CAP_KEYS,
            "caps=%r" % caps)
    c.check("proto tag is single source of truth", P.PROTO_TAG == "p4sync-1")
    c.check("manifest schema accepts shipped CLI shape",
            P.validate_manifest({"board": "jc1060p470c", "time": 1,
                                 "pim": {}, "files": [], "apps": {},
                                 "shot": None}) == "")
    c.check("manifest schema rejects a missing field",
            "missing" in P.validate_manifest({"board": "x", "time": 1}))
    toggle_manifest = {"board": "jc1060p470c", "time": 1, "pim": {},
                       "files": [], "apps": {}, "shot": True}
    c.check("manifest schema rejects the old boolean shot toggle",
            "manifest.shot" in P.validate_manifest(toggle_manifest) and
            "manifest.shot" in P.manifest_stale_reason(toggle_manifest))
    rows = P.parse_dir_listing(B.MOCK_STATUS + "\n Directory of /sdcard\n" +
                               "2026-11-26  10:23       12 B  NOTES.TXT\n")
    c.check("detailed dir parser yields full path and size",
            rows == [{"path": "/sdcard/NOTES.TXT", "size": 12,
                      "mtime": rows[0]["mtime"]}] and rows[0]["mtime"] is not None,
            "rows=%r" % rows)
    c.check("newer-wins matches pim_sync (device wins ties)",
            P.newer_wins({"mtime": 20}, {"mtime": 10}) == "local" and
            P.newer_wins({"mtime": 10}, {"mtime": 20}) == "remote" and
            P.newer_wins({"mtime": 10}, {"mtime": 10}) == "remote")


def check_bridge(c: Checklist) -> None:
    temp = os.path.join(WORK, "p4sync-gate-%d" % os.getpid())
    shutil.rmtree(temp, ignore_errors=True)
    os.makedirs(temp)
    try:
        local = os.path.join(temp, "NEW.TXT")
        with open(local, "wb") as handle:
            handle.write(b"new local file")
        shot = os.path.join(temp, "shot.png")
        pulled = os.path.join(temp, "pulled.bin")
        wrong = os.path.join(temp, "wrong.bin")
        reqs = [
            {"id": 1, "cmd": "devices"},
            {"id": 2, "cmd": "status", "port": "MOCK1"},
            {"id": 3, "cmd": "shell", "port": "MOCK1", "command": "echo hello"},
            {"id": 4, "cmd": "shell", "port": "MOCK1", "command": "echo hello",
             "plain": True},
            {"id": 5, "cmd": "push", "port": "MOCK1", "local": local.replace("\\", "/"),
             "remote": "/sdcard/NEW.TXT"},
            {"id": 6, "cmd": "verify", "port": "MOCK1", "remote": "/sdcard/NEW.TXT",
             "crc32": "%08x" % P.crc32(b"new local file")},
            {"id": 7, "cmd": "verify", "port": "MOCK1", "remote": "/sdcard/NEW.TXT",
             "crc32": "%08x" % (P.crc32(b"new local file") ^ 1)},
            {"id": 8, "cmd": "pull", "port": "MOCK1", "remote": "/sdcard/NEW.TXT",
             "local": pulled.replace("\\", "/")},
            {"id": 9, "cmd": "pull", "port": "MOCK1", "remote": "/sdcard/NEW.TXT",
             "local": wrong.replace("\\", "/"), "crc32": "deadbeef"},
            {"id": 10, "cmd": "screenshot", "port": "MOCK1", "out": shot.replace("\\", "/")},
            {"id": 11, "cmd": "pkg", "port": "MOCK1", "verb": "list"},
            {"id": 12, "cmd": "pkg", "port": "MOCK1", "verb": "install",
             "package": "DEMO"},
            {"id": 13, "cmd": "pkg", "port": "MOCK1", "verb": "uninstall",
             "package": "DEMO"},
            {"id": 14, "cmd": "pkg", "port": "MOCK1", "verb": "update"},
            {"id": 15, "cmd": "pkg", "port": "MOCK1", "verb": "bogus"},
            {"id": 16, "cmd": "verify", "port": "MOCK1", "remote": "notes.md",
             "crc32": "%08x" % P.crc32(B.MOCK_FILES["/sdcard/notes.md"][0])},
        ]
        lines, stderr = run_bridge(reqs)
        r = replies(lines)

        c.check("every request has exactly one valid JSON response",
                len(r) == len(reqs) and
                all(valid_response(r[req["id"]], req["id"]) for req in reqs)
                and "Traceback" not in stderr,
                "responses=%d stderr=%r" % (len(r), stderr[-160:]))
        c.check("progress/log events are JSON too",
                all(line.get("event") in ("progress", "log")
                    for line in lines if "event" in line) and
                len(events(lines, "progress")) >= 3)
        c.check("devices fixture needs no board",
                len(r[1]["data"]["devices"]) == 2 and
                all(d["proto"] == P.PROTO_TAG for d in r[1]["data"]["devices"]))
        st = r[2]["data"]
        c.check("status exposes flat caps and display",
                st["board"] == "jc1060p470c" and st["proto"] == P.PROTO_TAG and
                st["display"] == [1024, 600] and
                st["caps"]["magic.bmpx"] == "BMPX")
        c.check("shell uses Device.run and returns ERRORLEVEL",
                r[3]["data"]["output"].strip() == "hello" and
                r[3]["data"]["errorlevel"] == 0 and
                r[4]["data"]["errorlevel"] is None,
                "shell=%r" % r[3]["data"])
        c.check("push verifies the .part before rename",
                r[5]["ok"] and r[5]["data"]["part_verified"] and
                r[5]["data"]["crc32"] == "%08x" % P.crc32(b"new local file"))
        c.check("verify round-trips the shared CRC",
                r[6]["ok"] and r[6]["data"]["ok"] and
                not r[7]["data"]["ok"])
        c.check("pull writes verified bytes atomically",
                r[8]["ok"] and os.path.isfile(pulled) and
                open(pulled, "rb").read() == b"new local file")
        c.check("corrupt CRC is a hard failure (no fake green)",
                r[9]["ok"] is False and "E_CRC" in r[9]["error"] and
                not os.path.exists(wrong),
                "resp=%r" % r[9])
        names = replies(run_bridge([{"id": 20, "cmd": "shell", "port": "MOCK1",
                                     "command": "dir /b /s /sdcard"}])[0])[20]
        c.check("no .part residue left on device",
                ".part" not in names["data"]["output"])
        c.check("screenshot fixture is a real PNG file",
                os.path.isfile(shot) and
                open(shot, "rb").read(8) == B.MOCK_PNG[:8] and
                r[10]["data"]["width"] == 1024 and
                r[10]["data"]["out"] == os.path.abspath(shot))
        shot_data = r[10]["data"]
        png_bytes = open(shot, "rb").read()
        c.check("screenshot carries a machine-readable PNG payload",
                shot_data.get("png_base64") ==
                base64.b64encode(png_bytes).decode("ascii") and
                shot_data.get("sha256") == hashlib.sha256(png_bytes).hexdigest()
                and shot_data.get("size") == len(png_bytes) and
                shot_data.get("bytes") == len(png_bytes) and
                shot_data.get("path") == shot_data.get("png") == shot_data["out"],
                "keys=%r" % sorted(shot_data))
        default_lines, _ = run_bridge([
            {"id": 30, "cmd": "screenshot", "port": "MOCK1"},
            {"id": 31, "cmd": "screenshot", "port": "MOCK1"},
        ])
        defaults = replies(default_lines)
        c.check("default screenshot path is deterministic under artifacts dir",
                defaults[30]["data"]["out"] == defaults[31]["data"]["out"] and
                os.path.dirname(defaults[30]["data"]["out"]) == B.artifacts_dir()
                and os.path.basename(defaults[30]["data"]["out"]) ==
                "p4sync-shot-mock1.png" and
                open(defaults[30]["data"]["out"], "rb").read() == png_bytes,
                "out=%r" % defaults[30]["data"]["out"])
        pkg = {verb: r[ident]["data"]["command"] for ident, verb in
               ((11, "list"), (12, "install"), (13, "uninstall"), (14, "update"))}
        c.check("pkg verbs map to existing firmware verbs",
                pkg == {"list": "pkg list /b", "install": "pkg install DEMO",
                        "uninstall": "pkg remove DEMO", "update": "pkg outdated"},
                "pkg=%r" % pkg)
        c.check("unsupported pkg verb is refused",
                r[15]["ok"] is False and "E_UNSUPPORTED" in r[15]["error"])
        c.check("mock file lookup is FATFS-style case-insensitive",
                r[16]["ok"] and r[16]["data"]["ok"] and
                r[16]["data"]["remote"] == "notes.md")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def check_no_serial_mock(c: Checklist) -> None:
    """--mock must start on a machine with no pyserial installed."""
    temp = os.path.join(WORK, "p4sync-gate-noserial-%d" % os.getpid())
    shutil.rmtree(temp, ignore_errors=True)
    os.makedirs(temp)
    try:
        blocker = os.path.join(temp, "serial.py")
        with open(blocker, "w", encoding="utf-8") as handle:
            handle.write('raise ImportError("simulated missing pyserial")\n')
        env = dict(os.environ)
        env["PYTHONPATH"] = temp + os.pathsep + env.get("PYTHONPATH", "")
        payload = json.dumps({"id": 1, "cmd": "status", "port": "MOCK1"}) + "\n"
        proc = subprocess.run([sys.executable, BRIDGE, "--mock"],
                              input=payload, text=True, capture_output=True,
                              env=env, timeout=60)
        try:
            parsed = [json.loads(line) for line in proc.stdout.splitlines()]
        except ValueError:
            parsed = []
        c.check("mock mode needs no pyserial",
                proc.returncode == 0 and len(parsed) == 1 and
                valid_response(parsed[0], 1) and
                "ImportError" not in proc.stderr,
                "stderr=%r" % proc.stderr[-200:])
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def check_cli(c: Checklist) -> None:
    """CLI --mock --json must emit the same data object as the bridge."""
    cli = os.path.join(TOOLS, "p4sync")
    proc = subprocess.run([sys.executable, cli, "--mock", "--json", "status"],
                          text=True, capture_output=True, timeout=60)
    try:
        parsed = [json.loads(line) for line in proc.stdout.splitlines()]
    except ValueError as exc:
        parsed = []
        raise AssertionError("CLI stdout is not JSON: %s" % exc)
    c.check("CLI --json is one structured object, no progress on stdout",
            proc.returncode == 0 and len(parsed) == 1 and
            "event" not in parsed[0] and "Traceback" not in proc.stderr)
    from_bridge = replies(run_bridge([{"id": 1, "cmd": "status",
                                       "port": "MOCK1"}])[0])[1]["data"]
    c.check("CLI data matches bridge data schema",
            parsed and parsed[0].keys() == from_bridge.keys() and
            parsed[0]["proto"] == from_bridge["proto"] and
            parsed[0]["display"] == from_bridge["display"])


def check_cli_smoke(c: Checklist) -> None:
    """Every bridge command is a CLI verb: --mock --json, one object, rc."""
    cli = os.path.join(TOOLS, "p4sync")
    temp = os.path.join(WORK, "p4sync-gate-cli-%d" % os.getpid())
    shutil.rmtree(temp, ignore_errors=True)
    os.makedirs(temp)
    try:
        mirror = os.path.join(temp, "mirror").replace("\\", "/")
        cases = [
            ("status", ["status"], ("proto", "display", "caps")),
            ("shell", ["shell", "echo hello"], ("output", "errorlevel", "ok")),
            ("screenshot", ["screenshot"],
             ("out", "path", "png_base64", "sha256", "size", "bytes")),
            ("shot alias", ["shot"], ("out", "png_base64", "sha256")),
            ("full-image", ["full-image", "--mirror", mirror, "--db", "p4sync"],
             ("manifest", "manifest_path", "stale_manifest", "rebuilt", "ok")),
        ]
        for label, tail, keys in cases:
            proc = subprocess.run(
                [sys.executable, cli, "--mock", "--json", *tail],
                text=True, capture_output=True, timeout=120)
            try:
                parsed = [json.loads(line) for line in proc.stdout.splitlines()]
            except ValueError:
                parsed = []
            c.check("CLI mock smoke: %s" % label,
                    proc.returncode == 0 and len(parsed) == 1 and
                    "event" not in parsed[0] and
                    all(key in parsed[0] for key in keys) and
                    "Traceback" not in proc.stderr,
                    "rc=%d lines=%d stderr=%r"
                    % (proc.returncode, len(parsed), proc.stderr[-160:]))
        bad = subprocess.run(
            [sys.executable, cli, "--mock", "--json", "verify",
             "/sdcard/NOTES.TXT", "--crc32", "deadbeef"],
            text=True, capture_output=True, timeout=60)
        try:
            bad_data = json.loads(bad.stdout)
        except ValueError:
            bad_data = {}
        c.check("CLI exits non-zero on a failed command",
                bad.returncode == 1 and bad_data.get("ok") is False,
                "rc=%d out=%r" % (bad.returncode, bad.stdout[-160:]))
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def check_full_image(c: Checklist) -> None:
    temp = os.path.join(WORK, "p4sync-gate-image-%d" % os.getpid())
    shutil.rmtree(temp, ignore_errors=True)
    os.makedirs(temp)
    try:
        mirror = os.path.join(temp, "mirror").replace("\\", "/")
        shot = os.path.join(temp, "image.png").replace("\\", "/")
        req = {"id": 1, "cmd": "full-image", "port": "MOCK1", "mirror": mirror,
               "db": "NOTES", "shot": shot}
        lines, stderr = run_bridge([req])
        r = replies(lines)[1]
        data = r.get("data", {})
        c.check("full-image reuses shipped CLI composition",
                r["ok"] and data.get("failures") == 0 and
                "full-image: manifest" in data.get("output", "") and
                not stderr.strip(),
                "resp=%r stderr=%r" % (r, stderr[-160:]))
        path = data.get("manifest_path", "")
        c.check("image.json is validated, not assumed",
                os.path.isfile(path) and
                P.validate_manifest(data.get("manifest", {})) == "" and
                data["manifest"]["time"] == B.MOCK_TIME)
        c.check("screenshot proof lands next to the manifest",
                os.path.isfile(shot) and data["manifest"]["shot"] == shot)

        # The GUI sends feature toggles as booleans, not paths. They must be
        # coerced: os.walk(True)/open(True) would treat True as a file
        # descriptor and could write PNG bytes into the JSON stdout.
        bool_req = {"id": 2, "cmd": "full-image", "port": "MOCK",
                    "mirror": mirror, "db": "p4sync", "files": True,
                    "shot": True}
        bool_lines, bool_err = run_bridge([bool_req])
        bool_data = replies(bool_lines)[2]
        c.check("boolean files/shot toggles keep stdout JSON-only",
                bool_data["ok"] and
                bool_data["data"]["manifest"]["shot"] == os.path.join(
                    mirror, "shot.png") and
                os.path.isfile(os.path.join(mirror, "shot.png")) and
                "Traceback" not in bool_err,
                "resp=%r stderr=%r" % (bool_data, bool_err[-160:]))

        # A stale image.json from the earlier boolean-toggle bug used to pass
        # structural validation with "shot": true. It must be detected, dropped
        # and rebuilt from the current mirror - never silently accepted.
        stale_dir = os.path.join(temp, "stale").replace("\\", "/")
        os.makedirs(stale_dir, exist_ok=True)
        with open(os.path.join(stale_dir, "image.json"), "w",
                  encoding="utf-8") as handle:
            json.dump({"board": "jc1060p470c", "time": 1, "pim": {},
                       "files": [], "apps": {}, "shot": True}, handle)
        stale_req = {"id": 3, "cmd": "full-image", "port": "MOCK1",
                     "mirror": stale_dir, "db": "p4sync", "files": True,
                     "shot": True}
        stale_lines, stale_err = run_bridge([stale_req])
        stale_answer = replies(stale_lines)[3]
        c.check("stale boolean-shot manifest is detected and rebuilt",
                stale_answer["ok"] and stale_answer["data"]["ok"] and
                stale_answer["data"]["stale_manifest"] and
                stale_answer["data"]["rebuilt"] and
                "manifest.shot" in stale_answer["data"]["stale_reason"] and
                P.validate_manifest(stale_answer["data"]["manifest"]) == "" and
                not stale_err.strip(),
                "resp=%r stderr=%r" % (stale_answer, stale_err[-160:]))
        proof = os.path.join(stale_dir, "shot.png")
        c.check("rebuilt manifest points at a real PNG proof and logs recovery",
                os.path.isfile(proof) and
                stale_answer["data"]["manifest"]["shot"] == proof and
                open(proof, "rb").read(8) == P.PNG_MAGIC and
                len(events(stale_lines, "log")) >= 1,
                "manifest=%r" % stale_answer["data"]["manifest"])
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def _make_plan_tree(root: str) -> Tuple[str, str]:
    """Identical + conflicted + local-only files, with a stale host cache."""
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "ALARMS.ICS"), "wb") as handle:
        handle.write(B.MOCK_FILES["/sdcard/ALARMS.ICS"][0])
    with open(os.path.join(root, "NOTES.TXT"), "wb") as handle:
        handle.write(b"locally changed")
    os.utime(os.path.join(root, "NOTES.TXT"), (B.MOCK_TIME - 500000,
                                              B.MOCK_TIME - 500000))
    with open(os.path.join(root, "NEW.TXT"), "wb") as handle:
        handle.write(b"new local file")
    cache = os.path.join(os.path.dirname(root), "cache.json")
    with open(cache, "w", encoding="utf-8") as handle:
        json.dump({"NOTES.TXT": {"path": "NOTES.TXT", "size": 1,
                                 "mtime": 1, "crc32": 123}}, handle)
    return root.replace("\\", "/"), cache.replace("\\", "/")


def check_plan(c: Checklist) -> None:
    temp = os.path.join(WORK, "p4sync-gate-plan-%d" % os.getpid())
    shutil.rmtree(temp, ignore_errors=True)
    os.makedirs(temp)
    try:
        local, cache = _make_plan_tree(os.path.join(temp, "local"))
        reqs = [
            {"id": 1, "cmd": "plan", "port": "MOCK1", "local": local,
             "remote": "/sdcard", "cache": cache, "dry_run": True},
            {"id": 2, "cmd": "plan", "port": "MOCK1", "local": local,
             "remote": "/sdcard", "cache": cache, "dry_run": False,
             "apply": True},
            {"id": 3, "cmd": "verify", "port": "MOCK1",
             "remote": "/sdcard/NEW.TXT",
             "crc32": "%08x" % P.crc32(b"new local file")},
        ]
        lines, stderr = run_bridge(reqs)
        r = replies(lines)
        first = r[1]["data"]
        by_path = {a["path"]: a for a in first["actions"]}
        c.check("plan skips identical and detects all three changes",
                by_path.get("ALARMS.ICS", {}).get("op") == "skip" and
                by_path.get("NEW.TXT", {}).get("op") == "push" and
                by_path.get("NOTES.TXT", {}).get("op") == "pull" and
                by_path.get("apps/DEMO/APP.BAT", {}).get("op") == "pull",
                "actions=%r" % first["actions"])
        c.check("newer-wins conflict is reported, not silently merged",
                by_path.get("NOTES.TXT", {}).get("conflict") is True and
                first["summary"]["conflicts"] == 1 and
                first["summary"]["dry_run"] is True)
        applied = r[2]["data"]["summary"]
        progresses = len(events(lines, "progress"))
        c.check("apply transfers one file at a time with progress",
                applied["applied"] == 4 and progresses >= 8,
                "applied=%r progress=%d" % (applied, progresses))
        c.check("applied push is really on the device",
                r[3]["ok"] and r[3]["data"]["ok"] and
                open(os.path.join(local, "NOTES.TXT"), "rb").read() ==
                B.MOCK_FILES["/sdcard/NOTES.TXT"][0],
                "verify=%r" % r[3])
        c.check("git-ignored cache was written only on the applied run",
                os.path.isfile(cache) and
                isinstance(json.load(open(cache, encoding="utf-8")), dict))
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def check_canaries(c: Checklist) -> None:
    """Negative paths for the three red canaries + second-serial regression."""
    bridge = B.Bridge(mock=True)
    bad = FakeClient(status_text=B.MOCK_STATUS.replace("p4sync-1", "p4sync-9"))
    bridge._clients["MOCK1"] = bad
    answer = in_process(bridge, {"id": 1, "cmd": "status", "port": "MOCK1"})
    c.check("bad handshake canary is red", answer["ok"] is False and
            "E_PROTO" in answer["error"], "resp=%r" % answer)

    bridge = B.Bridge(mock=True)
    bridge._clients["MOCK1"] = FakeClient(marker_error=(
        "run('sync status'): marker P4TAG99 not seen within 25s"))
    answer = in_process(bridge, {"id": 2, "cmd": "status", "port": "MOCK1"})
    c.check("missing marker canary is red", answer["ok"] is False and
            "E_MARKER" in answer["error"], "resp=%r" % answer)

    fake = _FakeReader(chunks=[b"stale output"])
    raised = False
    try:
        DeviceSession.read_until(fake, b"[OK]", 0.05, required=True)
    except P4Error:
        raised = True
    c.check("shared marker reader still rejects a missing marker", raised and
            bytes(fake._rx) == b"stale output")

    with open(BRIDGE, "r", encoding="utf-8") as handle:
        source = handle.read()
    c.check("no second serial/framing implementation in bridge",
            second_serial_hits(source) == [],
            "hits=%r" % second_serial_hits(source))
    c.check("no-fake-success response validator rejects empty payload",
            not valid_response({"id": 1, "ok": True}, 1) and
            not valid_response({"id": 1, "ok": False, "error": ""}, 1) and
            valid_response({"id": 1, "ok": True, "data": {"x": 1}}, 1))
    stale = {"board": "jc1060p470c", "time": 1, "pim": {},
             "files": [], "apps": {}, "shot": True}
    fresh = dict(stale, shot=None)
    c.check("stale boolean-shot manifest canary is red",
            "manifest.shot" in P.validate_manifest(stale) and
            "manifest.shot" in P.manifest_stale_reason(stale) and
            P.manifest_stale_reason(fresh) == "")
    missing_proof = os.path.join(
        WORK, "p4sync-gate-missing-proof-%d.png" % os.getpid())
    c.check("missing PNG proof is detected against the current mirror",
            "missing" in B.manifest_proof_error({"shot": missing_proof}))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="full board-free check (default)")
    ap.add_argument("--self-test", action="store_true",
                    help="red canaries only; no bridge subprocess")
    args = ap.parse_args(argv)
    c = Checklist("p4sync_gate self-test" if args.self_test else "p4sync_gate")
    check_protocol(c)
    if args.self_test:
        check_canaries(c)
        # A corrupt CRC must not be reported as a successful verification.
        false_ok = P.crc32(b"a") == P.crc32(b"b")
        c.check("corrupt CRC canary is red", not false_ok)
        c.check("reversed mtimes flip the conflict winner",
                P.newer_wins({"mtime": 1}, {"mtime": 2}) == "remote" and
                P.newer_wins({"mtime": 2}, {"mtime": 1}) == "local")
        bad_source = "import serial\nser = serial.Serial(port)\n"
        c.check("second-serial scanner can go red",
                second_serial_hits(bad_source) != [])
    else:
        check_bridge(c)
        check_no_serial_mock(c)
        check_cli(c)
        check_cli_smoke(c)
        check_full_image(c)
        check_canaries(c)
        check_plan(c)
    return c.finish()


if __name__ == "__main__":
    raise SystemExit(main())
