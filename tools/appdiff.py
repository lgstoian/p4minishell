# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""appdiff.py - behaviour-parity gate for the batch-app control-flow rewrite.

Drives every reference app with a fixed script and records a *normalised*
transcript (structured output only; dynamic values are masked) plus the app's
`ui targets` geometry. `--update` writes the baseline; the default compares the
current run against it and exits non-zero on any difference, so an app rewrite
cannot silently change behaviour.

Only the batch-app *control flow* is under test here, so the transcript is the
signal of record; screenshots are captured for review but not diffed.

Usage:
    python tools/appdiff.py COM3 --update     # capture baseline (current fw)
    python tools/appdiff.py COM3              # compare
    python tools/appdiff.py COM3 --only MOOD,PICS
"""
from __future__ import annotations

import argparse
import difflib
import os
import re
import sys
import time
from typing import List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p4test.device import Device  # noqa: E402

BASE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "appdiff", "baseline")

# Mirrors suites/s14_apps.py. (name, start_marker, done_marker, timeout)
NONSTOP = [
    ("BOUNCE", "[M-BOUNCE]", "[M-BOUNCE-DONE]", 220.0),
    ("GFXTOOL", "[M-GFXTOOL]", "[M-GFXTOOL-DONE]", 120.0),
    ("PLOT", "[M-PLOT]", "[M-PLOT-DONE]", 60.0),
    ("UITEST", "[M-UITEST]", "[M-UITEST-DONE]", 60.0),
    ("CONTROL", "[M-CONTROL]", "[M-CONTROL-DONE]", 60.0),
]
# Interactive apps: (name, start, exit_line, exit_marker, open_wait, shot)
DRIVE = [
    ("ADVENT", "[M-ADV]", "quit", "[M-ADV-QUIT]", 2.5, True),
    ("MOOD", "[M-MOOD]", "5", None, 5.0, False),
    ("TCMD", "[M-TCMD]", "q", "[M-TCMD-DONE]", 3.0, False),
    ("ELITE", "[M-ELITE-DOCKED]", "5", "[M-ELITE-DONE]", 3.0, False),
    ("PICS", "[M-PICS]", "5", None, 4.0, True),
    ("PALMTOP", "[M-PALM]", "7", None, 4.0, True),
    ("DIAG", "[M-DIAG]", "9", "[M-DIAG-DONE]", 4.0, False),
    ("COMPANION", "[C-MENU]", "6", "[C-EXIT]", 2.0, True),
]
ALL_APPS = [n for n, *_ in NONSTOP] + [n for n, *_ in DRIVE] + ["SNAKE", "NOTES"]

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]")
_P4TAG = re.compile(r"P4TAG\d+X\d+")
_LOGTS = re.compile(r"^[IWE] \(\d+\) ")
_CLOCK = re.compile(r"\b\d{1,2}:\d{2}(:\d{2})?\b")
_DATE = re.compile(r"\b\d{4}-\d{2}-\d{2}\b")
_HEX = re.compile(r"\b0x[0-9a-fA-F]+\b")
_IP = re.compile(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b")
_DIGITS = re.compile(r"\d+")
# The line-buffered prompt can land on the same line as app output depending on
# timing; strip it so it cannot create a false diff.
_PROMPT = re.compile(r"PS [^>\r\n]*>\s?")

# Per-app dynamic lines: live game loops emit a variable number of frame/variable
# dumps between runs, which is timing, not control flow. Drop them so the gate
# compares the deterministic structure (intro, prompts, markers, exit).
_DROP = {
    "SNAKE": (
        re.compile(r"^\s*-\w+="),
        re.compile(r"^\s*(dir|nh|q|t|hh|s\d|dc|d)="),
        re.compile(r"^Move\? "),
    ),
}


def normalise(text: str, app: Optional[str] = None) -> str:
    """Mask dynamic values and collapse repeated lines, keeping line structure.

    Every digit run is masked, so heap/time/random/uptime variance cannot hide a
    control-flow change: a rewrite that adds, drops, reorders or changes a line
    still shows up as a transcript diff.
    """
    drop = _DROP.get(app or "", ())
    lines: List[str] = []
    for raw in text.splitlines():
        line = _ANSI.sub("", raw).rstrip()
        line = _PROMPT.sub("", line)
        line = _LOGTS.sub("", line)
        line = _P4TAG.sub("P4TAG", line)
        line = _DATE.sub("DATE", line)
        line = _CLOCK.sub("HH:MM", line)
        line = _IP.sub("IP", line)
        line = _HEX.sub("0xN", line)
        line = _DIGITS.sub("N", line)
        line = re.sub(r"[ \t]+", " ", line).strip()
        if not line:
            continue
        if any(p.search(line) for p in drop):
            continue
        lines.append(line)

    # Collapse runs of identical consecutive lines (animation frame spam).
    out: List[str] = []
    i = 0
    while i < len(lines):
        j = i
        while j + 1 < len(lines) and lines[j + 1] == lines[i]:
            j += 1
        if j > i:
            out.append("%s   (xN)" % lines[i])
        else:
            out.append(lines[i])
        i = j + 1
    return "\n".join(out) + "\n"


def _drain(dev: Device, seconds: float) -> str:
    try:
        return dev.read_for(seconds)
    except Exception:  # noqa: BLE001
        return ""


def _targets_text(dev: Device) -> str:
    dev.session.reset_input()
    dev.session.write_line("ui targets")
    raw = _drain(dev, 2.2)
    names = []
    for line in raw.splitlines():
        m = re.match(r"^\s*\d+\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)(?:\s+(.*))?$", line)
        if m:
            name = (m.group(5) or "").strip()
            name = _DIGITS.sub("N", name)
            names.append(name)
    return "\n".join(names) + "\n"


def run_app(dev: Device, name: str) -> Tuple[str, str]:
    session = dev.session
    session.reset_input()
    if name in [n for n, *_ in NONSTOP]:
        _, start, done, timeout = next(e for e in NONSTOP if e[0] == name)
        session.write_line("launch %s" % name)
        out = session.read_until(start.encode(), 60).decode("utf-8", "replace")
        out += session.read_until(done.encode(), timeout).decode("utf-8", "replace")
        targets = ""
    elif name == "SNAKE":
        session.write_line("launch SNAKE")
        out = session.read_until(b"[M-SNAKE]", 60).decode("utf-8", "replace")
        out += _drain(dev, 3.0)
        session.write_line("q")
        out += session.read_until(b"[M-SNAKE-DONE]", 40).decode("utf-8", "replace")
        targets = ""
    elif name == "NOTES":
        session.write_line("launch NOTES")
        out = session.read_until(b"[M-NOTE]", 60).decode("utf-8", "replace")
        out += _drain(dev, 12.0)
        session.write_line("ok")
        out += _drain(dev, 2.0)
        targets = _targets_text(dev)
        session.write_line("8")
        out += _drain(dev, 2.0)
        session.write_line("8")
        out += _drain(dev, 2.0)
    else:
        _, start, exit_line, exit_marker, open_wait, _shot = next(
            e for e in DRIVE if e[0] == name)
        session.write_line("launch %s" % name)
        out = session.read_until(start.encode(), 60).decode("utf-8", "replace")
        out += _drain(dev, open_wait)
        if name in ("PICS", "PALMTOP", "COMPANION", "MOOD"):
            targets = _targets_text(dev)
        else:
            targets = ""
        if exit_line is not None:
            session.write_line(exit_line)
        if exit_marker is not None:
            out += session.read_until(exit_marker.encode(), 40).decode("utf-8", "replace")
        else:
            out += _drain(dev, 3.0)
    # Return the shell and prove it is alive before the next app.
    for _ in range(6):
        session.write(b"\x03\r\n")
        time.sleep(0.6)
        try:
            if "P4APPOK" in dev.run("echo P4APPOK", timeout=12):
                break
        except Exception:  # noqa: BLE001
            continue
    try:
        dev.run("draw close", timeout=10)
    except Exception:  # noqa: BLE001
        pass
    try:
        dev.run("keyboard hide", timeout=10)
    except Exception:  # noqa: BLE001
        pass
    return normalise(out, name), targets


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM3")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    apps = ALL_APPS
    if args.only:
        want = {a.strip().upper() for a in args.only.split(",")}
        apps = [a for a in apps if a in want]

    os.makedirs(BASE_DIR, exist_ok=True)
    dev = Device(args.port)
    try:
        dev.run("draw close", timeout=10)
        # Pin the on-screen keyboard to hidden: its visibility is persisted UI
        # state (a `config OSK` / boot setting), so a fresh board or an erased
        # NVS would otherwise make `ui state`/`ui targets` differ for reasons
        # unrelated to app control flow.
        dev.run("keyboard hide", timeout=10)
        dev.run("cls", timeout=10)
        failures = 0
        for name in apps:
            t0 = time.time()
            got, targets = run_app(dev, name)
            path = os.path.join(BASE_DIR, name + ".transcript.txt")
            tpath = os.path.join(BASE_DIR, name + ".targets.txt")
            if args.update:
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(got)
                with open(tpath, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(targets)
                print("[%-9s] baseline updated (%.0fs)" % (name, time.time() - t0), flush=True)
                continue
            want = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
            want_t = open(tpath, encoding="utf-8").read() if os.path.exists(tpath) else ""
            if got != want:
                failures += 1
                print("[%-9s] TRANSCRIPT DIFF" % name, flush=True)
                for line in difflib.unified_diff(
                        want.splitlines(), got.splitlines(),
                        fromfile="baseline", tofile="current", lineterm=""):
                    print("   " + line)
            if targets != want_t:
                failures += 1
                print("[%-9s] TARGETS DIFF" % name, flush=True)
                for line in difflib.unified_diff(
                        want_t.splitlines(), targets.splitlines(),
                        fromfile="baseline", tofile="current", lineterm=""):
                    print("   " + line)
            if got == want and targets == want_t:
                print("[%-9s] ok (%.0fs)" % (name, time.time() - t0), flush=True)
        print("appdiff: %d app(s), %d diff(s)" % (len(apps), failures))
        return 1 if failures else 0
    finally:
        dev.close()


if __name__ == "__main__":
    raise SystemExit(main())
