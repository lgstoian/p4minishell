#!/usr/bin/env python3
"""unit_run.py - flash the test project, capture Unity results.

Usage: python unit_run.py [COMx]
"""
import re
import sys
import time

sys.path.insert(0, "tools")
from shell_session import open_port, hard_reset


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    # open_port() no longer reboots, so reset explicitly so the flashed test
    # image runs from the start and Unity prints its summaries.
    hard_reset(port)
    ser = open_port(port)
    ser.timeout = 0.5
    # Read continuously: the OS serial buffer is small, so sleeping through
    # the run loses early suites. 220 s covers boot + the full Unity run.
    data = b""
    end = time.time() + 220
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
    ser.close()
    text = re.sub(r"\x1b\[[0-9;]*m", "", data.decode("utf-8", errors="replace"))
    # Print only the summary region.
    lines = text.splitlines()
    start = 0
    for i, l in enumerate(lines):
        if "Unity" in l or "PASS" in l or "FAIL" in l or "IGNORED" in l:
            start = max(0, i - 2)
            break
    tail = lines[start:]
    # Condense: show failures + final summary.
    fails = [l for l in lines if "FAIL" in l]
    summ = [l for l in lines if re.search(r"\d+ Tests? .*Failures?|\d+ Ignored", l)]
    print("FAIL lines: %d" % len(fails), flush=True)
    for l in fails[:20]:
        print("  " + l.strip()[:160], flush=True)
    print("--- summary (all suites) ---", flush=True)
    total = 0
    for l in summ:
        print("  " + l.strip()[:160], flush=True)
        m = re.search(r"(\d+) Tests?", l)
        if m:
            total += int(m.group(1))
    print("TOTAL tests: %d" % total, flush=True)
    if not summ:
        print("(no summary found; last 15 lines:)", flush=True)
        for l in lines[-15:]:
            print("  " + l.strip()[:160], flush=True)


main()
