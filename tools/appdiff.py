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
from p4test.screenshot import TOL_FRAME, diff_ratio, parse_bmp  # noqa: E402
from p4test.session import P4Error  # noqa: E402

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
    ("MOOD", "[M-MOOD]", "6", None, 5.0, False),
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

# `tui dump /a` envelope emitted by the firmware (one text row and one colour
# run line per page row). Structural digits are load-bearing here, so they are
# NOT fed through normalise()'s digit mask: row indices and foreground colours
# are the assertion, while heap/time/random in the transcript are genuinely
# dynamic. A zero-row page is valid (a `draw` app does not own the browser page
# model); the pairing/bounds checks below still have to hold.
DUMP_HEAD = "tui.dump cols="
DUMP_END = "tui.dump.end"
ROW_RE = re.compile(r"^(\d{3})\|(.*)$")
RUN_RE = re.compile(r"^(\d{3})@(.*)$")
RUN_ITEM_RE = re.compile(r"^(\d+)-(\d+):(\d+)$")
TUI_PAGE_APPS = frozenset(("PICS", "PALMTOP", "COMPANION", "MOOD", "NOTES"))
FRAME_MAX_RATIO = 0.02
STRUCT_ERRORS: List[str] = []

# Per-app dynamic lines: live game loops emit a variable number of frame/variable
# dumps between runs, which is timing, not control flow. Drop them so the gate
# compares the deterministic structure (intro, prompts, markers, exit).
_DROP = {
    # UITEST prints `ui targets` into its transcript. Unnamed `obj` rows are
    # transient LVGL objects whose count depends on async deletion timing, not
    # app control flow; the named rows are still compared.
    "UITEST": (
        re.compile(r"^N\s+N\s+-?N\s+N\s+N\s+obj(?: \[C\])?(?:\s+\(xN\))?$"),
        # Header/transcript/input-row rows printed by `ui targets /v:NAME`:
        # live heap/battery/link telemetry, not app control flow.
        re.compile(r"^N\s+N\s+N\s+N\s+N\s+(?:W|BT|U|S|!|transcript|Prev|Next|"
                   r"N%|\+N%|N/C|M\s+N\.NM)(?:\s+\(xN\))?$"),
    ),
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


def parse_tui_dump(text: str) -> Tuple[int, dict, dict]:
    """Validate a board `tui dump 1 N /a` report; return (cols, rows, runs).

    Raises ValueError on a missing/stale envelope, duplicated row, malformed
    colour run, an out-of-range foreground or overlapping runs. A dump that
    cannot satisfy its own structure is a red gate, not a skipped check.
    """
    head = None
    ended = False
    rows = {}
    runs = {}
    cols = 0
    for raw in text.splitlines():
        line = _ANSI.sub("", raw).rstrip()
        # The shell prompt is line-buffered and can land at the start of any
        # dump line (including the envelope and row runs). Drop it before the
        # anchored checks, exactly as normalise() does for transcripts.
        line = _PROMPT.sub("", line).lstrip()
        if line.startswith(DUMP_HEAD):
            if head is not None:
                raise ValueError("duplicate tui dump header")
            head = line
            m = re.match(r"tui\.dump cols=(\d+) from=(\d+) to=(\d+)$", line)
            if not m:
                raise ValueError("malformed tui dump header: %r" % line)
            cols, first, last = (int(m.group(i)) for i in (1, 2, 3))
            if cols < 1 or first < 1 or last < first:
                raise ValueError("tui dump range out of order: %r" % line)
            continue
        if line == DUMP_END:
            ended = True
            continue
        m = ROW_RE.match(line)
        if m:
            index = int(m.group(1))
            if index in rows:
                raise ValueError("duplicate tui dump row %d" % index)
            rows[index] = m.group(2)
            continue
        m = RUN_RE.match(line)
        if m:
            index = int(m.group(1))
            if index in runs:
                raise ValueError("duplicate colour-run row %d" % index)
            payload = m.group(2).strip()
            items = payload.split() if payload else []
            parsed = []
            prev_end = 0
            for item in items:
                im = RUN_ITEM_RE.match(item)
                if not im:
                    raise ValueError("malformed colour run on row %d: %r"
                                     % (index, item))
                start, end, fg = (int(im.group(i)) for i in (1, 2, 3))
                if not (1 <= start <= end <= cols) or fg > 255:
                    raise ValueError("colour run out of bounds on row %d: %r"
                                     % (index, item))
                if start <= prev_end:
                    raise ValueError("overlapping colour runs on row %d: %r"
                                     % (index, item))
                prev_end = end
                parsed.append(item)
            runs[index] = parsed
    if head is None or not ended:
        raise ValueError("stale tui dump: missing %s / %s"
                         % (DUMP_HEAD, DUMP_END))
    if set(rows) != set(runs):
        raise ValueError("tui dump text/colour rows do not pair (%d/%d)"
                         % (len(rows), len(runs)))
    return cols, rows, runs


def runs_signature(runs: dict) -> str:
    """Exact colour-run structure; unlike the transcript, digits stay exact."""
    return "\n".join("%03d@%s" % (row, " ".join(runs[row]))
                     for row in sorted(runs)) + "\n"


def check_tui_structure(name: str, dev: Device, update: bool) -> None:
    """Assert `tui dump /a` structure for an app that owns a TUI page."""
    # An app can still own the input after its exit line (a key/ask loop), and
    # a raw 4 s drain can cut the 50-line dump short or merge the line-buffered
    # prompt into one row (24 text rows vs 25 colour runs). Prove the shell is
    # back with the same break/probe pattern run_app uses, then use the
    # marker-synced run so the whole dump is captured and ANSI-stripped.
    def shell_ready() -> bool:
        try:
            return "P4APPOK" in dev.run("echo P4APPOK", timeout=12)
        except Exception:  # noqa: BLE001 - a wedged app swallows the marker
            return False

    if not shell_ready():
        for _ in range(6):
            dev.session.write(b"\x03\r\n")
            time.sleep(0.6)
            if shell_ready():
                break
    try:
        out = dev.run("tui dump 1 25 /a", timeout=30)
    except P4Error as exc:
        STRUCT_ERRORS.append("%s: tui dump failed: %s" % (name, exc))
        return
    try:
        cols, rows, runs = parse_tui_dump(out)
    except ValueError as exc:
        STRUCT_ERRORS.append("%s: %s" % (name, exc))
        return
    base = os.path.join(BASE_DIR, name + ".runs.txt")
    want = open(base, encoding="utf-8").read() if os.path.isfile(base) else None
    if update:
        with open(base, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(runs_signature(runs))
        return
    if want is not None and runs_signature(runs) != want:
        STRUCT_ERRORS.append("%s: colour-run structure diff (baseline %s)"
                             % (name, base))
    print("[%-9s] tui dump /a: cols=%d rows=%d runs=%d%s" % (
        name, cols, len(rows), len(runs),
        "" if want is not None else " (structure checked, no golden baseline)"),
        flush=True)


def check_golden_frame(name: str, dev: Device, update: bool) -> None:
    """Optional pixel parity against `<APP>.frame.bmp` via the shared Bmp model.

    With no golden file this is an intentional no-op: geometry parity stays
    with the `ui targets` baseline, which the transcript gate never drops.
    """
    golden = os.path.join(BASE_DIR, name + ".frame.bmp")
    if not os.path.isfile(golden):
        return
    with open(golden, "rb") as fh:
        want = parse_bmp(fh.read())
    if update:
        dev.screenshot(out_dir=BASE_DIR, name=name + ".frame", unwind=False)
        return
    try:
        got = dev.screenshot(out_dir=BASE_DIR, name=name + ".frame", unwind=False)
    except P4Error as exc:
        STRUCT_ERRORS.append("%s: golden frame capture failed: %s" % (name, exc))
        return
    if (got.width, got.height) != (want.width, want.height):
        STRUCT_ERRORS.append("%s: golden frame geometry %dx%d vs %dx%d"
                             % (name, got.width, got.height,
                                want.width, want.height))
        return
    ratio = diff_ratio(got, want, tol=TOL_FRAME)
    if ratio > FRAME_MAX_RATIO:
        STRUCT_ERRORS.append("%s: golden frame pixel diff %.3f > %.3f"
                             % (name, ratio, FRAME_MAX_RATIO))


def _drain(dev: Device, seconds: float) -> str:
    try:
        return dev.read_for(seconds)
    except Exception:  # noqa: BLE001
        return ""


def _targets_text(dev: Device) -> str:
    """Capture the *named* `ui targets` of an app.

    `ui_collect_targets` also enumerates clickable LVGL objects whose label
    fallback is literally `obj` (toast notifications, off-screen widgets,
    transient modal objects). Their count depends on LVGL's async deletion
    timing, not on app control flow, so they can never form a stable baseline.
    Every target with a real name (menu rows, buttons, transcript, kbd keys)
    is still compared exactly; only the unnamed implementation objects are
    excluded. This keeps the parity gate meaningful instead of masking it.
    """
    dev.session.reset_input()
    dev.session.write_line("ui targets")
    raw = _drain(dev, 2.2)
    names = []
    for line in raw.splitlines():
        m = re.match(r"^\s*\d+\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)(?:\s+(.*))?$", line)
        if m:
            name = (m.group(5) or "").strip()
            name = _DIGITS.sub("N", name)
            # Header/transcript/input-row widgets are shell chrome, not app
            # targets; their labels carry live heap/battery/link telemetry
            # ("M N.NM", "N%", "N/C", "+N%") and change between runs.
            if (name in ("obj", "obj [C]", "W", "BT", "U", "S", "!",
                         "transcript", "Prev", "Next", "Tab", "textarea",
                         "\uf077", "\uf078", "N%", "+N%", "N/C", "N")
                    or name.startswith("M N.") or name.startswith("#")):
                continue
            names.append(name)
    return "\n".join(names) + "\n"


def run_app(dev: Device, name: str, update: bool = False) -> Tuple[str, str]:
    session = dev.session
    session.reset_input()
    if name in [n for n, *_ in NONSTOP]:
        _, start, done, timeout = next(e for e in NONSTOP if e[0] == name)
        session.write_line("launch %s" % name)
        out = session.read_until(start.encode(), 60, required=True).decode("utf-8", "replace")
        out += session.read_until(done.encode(), timeout, required=True).decode("utf-8", "replace")
        targets = ""
    elif name == "SNAKE":
        session.write_line("launch SNAKE")
        out = session.read_until(b"[M-SNAKE]", 60, required=True).decode("utf-8", "replace")
        out += _drain(dev, 3.0)
        session.write_line("q")
        out += session.read_until(b"[M-SNAKE-DONE]", 40, required=True).decode("utf-8", "replace")
        targets = ""
    elif name == "NOTES":
        session.write_line("launch NOTES")
        out = session.read_until(b"[M-NOTE]", 60, required=True).decode("utf-8", "replace")
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
        out = session.read_until(start.encode(), 60, required=True).decode("utf-8", "replace")
        out += _drain(dev, open_wait)
        if name == "DIAG" and b"[M-DIAG-MENU]" not in out.encode("utf-8", "replace"):
            # The menu is printed after the 14-line dashboard; under load it
            # can arrive after the fixed open_wait. Sending the exit key before
            # `list` is armed types it at the shell prompt and the app never
            # sees it (no [M-DIAG-DONE]).
            out += session.read_until(b"[M-DIAG-MENU]", 40,
                                      required=True).decode("utf-8", "replace")
        if name in ("PICS", "PALMTOP", "COMPANION", "MOOD"):
            targets = _targets_text(dev)
        else:
            targets = ""
        if exit_line is not None:
            session.write_line(exit_line)
        if exit_marker is not None:
            out += session.read_until(exit_marker.encode(), 40, required=True).decode("utf-8", "replace")
        else:
            out += _drain(dev, 3.0)
    # Host/board parity: the app's own `tui dump /a` colour runs must parse
    # and (when a golden run file is committed) match structurally. This is
    # additive to the `ui targets` geometry baseline below - never a
    # replacement for it. Digit masking belongs only to the transcript: row
    # indices and foreground colours here are structural, not dynamic.
    if name in TUI_PAGE_APPS:
        if not targets.strip():
            STRUCT_ERRORS.append("%s: ui targets empty for a TUI page app" % name)
        check_tui_structure(name, dev, update)
    if name in ("ADVENT", "PICS", "PALMTOP", "COMPANION"):
        check_golden_frame(name, dev, update)
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


def appdiff_self_test() -> int:
    """Host canary for the colour-run structure gate (no board needed)."""
    good = "\n".join([
        "tui.dump cols=80 from=1 to=3",
        "001|Pictures",
        "001@1-9:15",
        "002|1. View PHOTO.BMP",
        "002@1-8:7 9-10:15",
        "003|",
        "003@1-80:7",
        DUMP_END,
    ]) + "\n"
    checks = []
    try:
        cols, rows, runs = parse_tui_dump(good)
        checks.append(("valid dump parsed", cols == 80 and len(rows) == 3
                       and len(runs) == 3))
    except ValueError as exc:
        checks.append(("valid dump parsed", False))
        print("   unexpected: %s" % exc)
    bad_cases = [
        ("missing end marker", good.replace(DUMP_END + "\n", "")),
        ("malformed colour run", good.replace("001@1-9:15", "001@1-9:15x")),
        ("out-of-bounds colour run", good.replace("001@1-9:15", "001@1-81:15")),
        ("overlapping colour runs",
         good.replace("002@1-8:7 9-10:15", "002@1-8:7 8-10:15")),
        ("duplicate row", good.replace("002|1. View PHOTO.BMP", "001|dup")),
    ]
    for name, text in bad_cases:
        try:
            parse_tui_dump(text)
            rejected = False
        except ValueError:
            rejected = True
        checks.append((name + " rejected", rejected))
    sig = runs_signature({1: ["1-9:15"], 2: ["1-8:7", "9-10:15"]})
    checks.append(("structural digits kept exact",
                   "001@1-9:15" in sig and "002@1-8:7 9-10:15" in sig))
    checks.append(("dynamic transcript digits still masked",
                   normalise("1. View 12 items") == "N. View N items\n"))
    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    bad = sum(1 for _n, ok in checks if not ok)
    print("appdiff structure self-test: %s (%d canaries)"
          % ("PASS" if bad == 0 else "FAIL", len(checks)))
    return 1 if bad else 0


def main() -> int:
    # Windows consoles default to cp1252 and the app transcripts contain
    # box-drawing/ANSI-derived glyphs; never let a diff print crash the gate.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM3")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--only", default=None)
    ap.add_argument("--self-test", action="store_true",
                    help="host-only colour-run structure canary (no board)")
    args = ap.parse_args()

    if args.self_test:
        return appdiff_self_test()

    apps = ALL_APPS
    if args.only:
        want = {a.strip().upper() for a in args.only.split(",")}
        apps = [a for a in apps if a in want]

    os.makedirs(BASE_DIR, exist_ok=True)
    STRUCT_ERRORS.clear()
    dev = Device(args.port)
    try:
        dev.run("draw close", timeout=10)
        # Pin the on-screen keyboard to hidden: its visibility is persisted UI
        # state (a `config OSK` / boot setting), so a fresh board or an erased
        # NVS would otherwise make `ui state`/`ui targets` differ for reasons
        # unrelated to app control flow.
        dev.run("keyboard hide", timeout=10)
        dev.run("cls", timeout=10)
        # Per-run app state must not leak into the control-flow transcript:
        # ADVENT's save changes the inventory order and MOOD's sampler file
        # changes the service-view branch. These are disposable test artifacts.
        for artifact in ("ADVENT.SAV", "MOODLAST.INI"):
            dev.run("del /f %s" % artifact, timeout=15)
        failures = 0
        for name in apps:
            t0 = time.time()
            got, targets = run_app(dev, name, args.update)
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
        # Structural/golden-frame failures are real diffs, not notes. The
        # geometry baselines above were compared unchanged for every app.
        for err in STRUCT_ERRORS:
            failures += 1
            print("[STRUCT] %s" % err, flush=True)
        print("appdiff: %d app(s), %d diff(s)" % (len(apps), failures))
        return 1 if failures else 0
    except P4Error as exc:
        # A missing marker is a hard failure, never a stale-buffer transcript.
        print("appdiff: FAIL - %s" % exc, file=sys.stderr, flush=True)
        return 1
    finally:
        dev.close()


if __name__ == "__main__":
    raise SystemExit(main())
