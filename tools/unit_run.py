#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""unit_run.py - reset the board and capture Unity results from the test app.

The test image must already be flashed (`cd test; idf.py build flash`). This
runner hard-resets, reads the whole run, aggregates every PER-SUITE summary
(Unity resets counters per UNITY_BEGIN), prints the failures, and returns a
non-zero exit code when any test fails or the run does not complete.

Usage: python unit_run.py [COMx]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_session import default_port, hard_reset, open_port  # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# "N Tests M Failures K Ignored" (singular/plural varies per Unity version).
SUMMARY = re.compile(
    r"(\d+)\s+Tests?\s+(\d+)\s+Failures?\s+(\d+)\s+Ignored", re.IGNORECASE)
DONE = "=== All tests completed ==="
READ_SECONDS = 240


def main() -> int:
    port = (sys.argv[1] if len(sys.argv) > 1
            else (os.environ.get("P4_PORT") or default_port()))
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

    print("--- per-suite summaries ---")
    for l in lines:
        if SUMMARY.search(l):
            print("  " + l.strip()[:140])
    if fail_lines:
        print("--- FAIL lines ---")
        for l in fail_lines[:30]:
            print("  " + l[:160])
    if panics:
        print("--- panic markers ---")
        for l in panics[:10]:
            print("  " + l[:160])

    print("TOTAL tests: %d  failures: %d  ignored: %d" % (tests, failures, ignored))
    print("completed: %s" % ("yes" if completed else "NO"))

    ok = completed and failures == 0 and tests > 0 and not panics
    print("UNIT RESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
