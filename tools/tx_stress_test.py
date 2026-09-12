#!/usr/bin/env python3
"""tx_stress_test.py - TX-pressure output-integrity guard for bugs.md O3.

Sends N numbered `echo` lines while deliberately pausing reads so the
USB-Serial/JTAG TX path builds backpressure, then verifies every numbered
line arrived exactly once. A missing line is the O3 single-output loss: the
IDF SOF connection monitor can falsely report "disconnected" for a few ms
under load, and the VFS/VDS mirror used to drop the whole line when it did.

Usage: python tx_stress_test.py [COMx] [lines]
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else default_port()
LINES = int(sys.argv[2]) if len(sys.argv) > 2 else 300

ANSI = re.compile(rb"\x1b\[[0-9;]*m")
PROMPT = re.compile(r"^PS \S*> ")
MARK_RE = re.compile(r"TXM\d{4}")


def collect(seen, buf):
    text = ANSI.sub(b"", buf).decode("utf-8", errors="replace")
    for raw in text.splitlines():
        s = raw.strip()
        # Strip any glued prompt prefixes (the trailing prompt has no newline,
        # so a following output line can share its physical line).
        while True:
            m = PROMPT.match(s)
            if not m:
                break
            s = s[m.end():].strip()
        # The command input echo contains " echo TXMnnnn"; the output line is
        # exactly the marker. Count only the latter.
        if MARK_RE.fullmatch(s):
            seen.add(int(s[3:]))


def main():
    ser = open_port(PORT, 115200, 1)
    time.sleep(15.0)  # boot quiesce
    ser.reset_input_buffer()

    seen = set()
    buf = b""
    # Small bursts: the async command queue is only 16 deep, so flooding it
    # drops *commands* (not a mirror loss). Bursts of 8 with a read pause
    # between them build TX backpressure without overflowing the queue.
    for base in range(0, LINES, 8):
        for i in range(base, min(base + 8, LINES)):
            ser.write(("echo TXM%04d\n" % i).encode())
        time.sleep(0.8)  # pause reads: let backpressure build
        end = time.time() + 2.5
        while time.time() < end:
            d = ser.read(65536)
            if not d:
                break
            buf += d
        collect(seen, buf)

    end = time.time() + 8
    while time.time() < end and len(seen) < LINES:
        d = ser.read(65536)
        if not d:
            time.sleep(0.1)
            continue
        buf += d
        collect(seen, buf)
    ser.close()

    missing = [i for i in range(LINES) if i not in seen]
    print("TX stress: %d/%d lines received" % (len(seen), LINES))
    if missing:
        print("  missing: %s" % missing[:20])
    print("RESULT %s" % ("OK" if not missing else "FAIL"))
    return 1 if missing else 0


sys.exit(main())
