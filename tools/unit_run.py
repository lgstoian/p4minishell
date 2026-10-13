#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""unit_run.py - reset the board and capture Unity results from the test app.

The test image must already be flashed (`cd test; idf.py build flash`). This
runner hard-resets, reads the whole run, aggregates every PER-SUITE summary
(Unity resets counters per UNITY_BEGIN), prints the failures, and returns a
non-zero exit code when any test fails or the run does not complete.

Usage: python unit_run.py [COMx] [COMy]
  One or two ports; two ports run concurrently with a merged verdict.
"""
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_session import default_port, hard_reset, open_port  # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# "N Tests M Failures K Ignored" (singular/plural varies per Unity version).
SUMMARY = re.compile(
    r"(\d+)\s+Tests?\s+(\d+)\s+Failures?\s+(\d+)\s+Ignored", re.IGNORECASE)
DONE = "=== All tests completed ==="
READ_SECONDS = 240


def run_one(port: str, out: list) -> None:
    print("unit_run: port=%s (resetting)" % port, flush=True)
    hard_reset(port)
    ser = open_port(port)
    ser.timeout = 0.5
    # Read continuously: the OS serial buffer is small, so sleeping through
    # the run loses early suites.
    data = b""
    end = time.time() + READ_SECONDS
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            if DONE.encode() in data:
                # allow the trailing newline to arrive
                time.sleep(0.3)
                break
    ser.close()

    text = ANSI.sub("", data.decode("utf-8", errors="replace"))
    lines = text.splitlines()

    summaries = [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
                 for m in (SUMMARY.search(l) for l in lines) if m]
    tests = sum(s[0] for s in summaries)
    failures = sum(s[1] for s in summaries)
    ignored = sum(s[2] for s in summaries)

    fail_lines = [l.strip() for l in lines if "FAIL" in l.upper()
                  and "Failures" not in l]
    completed = DONE in text
    panics = [l.strip() for l in lines
              if any(p in l for p in ("Guru Meditation", "Backtrace",
                                      "assert failed", "abort()"))]

    buf = ["--- %s per-suite summaries ---" % port]
    for l in lines:
        if SUMMARY.search(l):
            buf.append("  " + l.strip()[:140])
    if fail_lines:
        buf.append("--- %s FAIL lines ---" % port)
        for l in fail_lines[:30]:
            buf.append("  " + l[:160])
    if panics:
        buf.append("--- %s panic markers ---" % port)
        for l in panics[:10]:
            buf.append("  " + l[:160])

    buf.append("TOTAL %s tests: %d  failures: %d  ignored: %d"
               % (port, tests, failures, ignored))
    buf.append("completed %s: %s" % (port, "yes" if completed else "NO"))

    ok = completed and failures == 0 and tests > 0 and not panics
    buf.append("UNIT RESULT %s %s" % (port, "OK" if ok else "FAIL"))
    out.append((port, tests, failures, ignored, ok, buf))


def main() -> int:
    args = [a for a in sys.argv[1:] if a.upper().startswith("COM")]
    if not args:
        args = [(os.environ.get("P4_PORT") or default_port())]
    out: list = []
    threads = [threading.Thread(target=run_one, args=(p.upper(), out),
                                name="unit-%s" % p.upper(), daemon=True)
               for p in args[:2]]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    for _port, _t, _f, _i, _ok, buf in sorted(out):
        for line in buf:
            print(line, flush=True)
    failed = sum(1 for _p, _t, _f, _i, ok, _b in out if not ok)
    if len(out) > 1:
        cells = ["%s:%s" % (p, "OK" if ok else "FAIL")
                 for p, _t, _f, _i, ok, _b in sorted(out)]
        print("UNIT MERGED %s" % " ".join(cells), flush=True)
    print("UNIT RESULT %s" % ("OK" if failed == 0 else "FAIL"))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
