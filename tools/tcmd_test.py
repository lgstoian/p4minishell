#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tcmd_test.py - drive TCMD v3 (dual-pane grid) with refresh-marker sync.

Usage: python tcmd_test.py [COMx]
Loop: wait [M-TCMD-R] (frame armed), send one key+Enter, wait for the next
frame marker. Then q -> [M-TCMD-DONE]. Pairs keys with the marker so a
key that arrives between frames cannot desync the run.
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot

PANIC = [b"Guru Meditation", b"Backtrace", b"abort()", b"assert failed",
         b"Stack protection", b"Task watchdog"]


def wait_marker(sh, marker, timeout=60):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            buf += chunk
            if marker.encode() in buf:
                return buf
            for p in PANIC:
                if p in buf:
                    raise RuntimeError("PANIC %r" % p)
    return buf


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    sh.s.timeout = 0.5
    sh.s.reset_input_buffer()
    sh.s.write(b"TCMD\r\n")
    data = wait_marker(sh, "M-TCMD-R")
    print("launched=%s" % (b"M-TCMD-R" in data), flush=True)
    for key in [b"j", b"j", b"k", b"t", b"s", b"2", b"1"]:
        sh.s.write(key + b"\r\n")
        data = wait_marker(sh, "M-TCMD-R", timeout=45)
        print("key %s -> frame=%s" % (key.decode(), b"M-TCMD-R" in data), flush=True)
    sh.s.write(b"q\r\n")
    data = wait_marker(sh, "M-TCMD-DONE", timeout=45)
    print("done=%s" % (b"M-TCMD-DONE" in data), flush=True)
    if b"M-TCMD-DONE" not in data:
        print(data.decode("utf-8", errors="replace")[-500:], flush=True)
    sh.close()


main()
