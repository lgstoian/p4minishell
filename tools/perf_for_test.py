#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""perf_for_test.py - functional + timing check for the for-loop deferral.

Usage: python perf_for_test.py [COMx]
1. echo over serial still mirrored (host connected).
2. a for loop with 20 iterations emits all 20 lines and completes.
3. a for/set-a loop emits all assignments (behavior unchanged) and times it.
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import open_port


def cmd(ser, text, wait=0.5, timeout=30):
    ser.reset_input_buffer()
    t0 = time.time()
    ser.write((text + "\r\n").encode())
    time.sleep(wait)
    data = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
        else:
            if data:
                break
    return time.time() - t0, data.decode("utf-8", errors="replace")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    ser = open_port(port)
    time.sleep(12)
    while True:
        if not ser.read(65536):
            break
    dt, out = cmd(ser, "echo MIRROR-CHECK")
    print("echo mirrored:", "MIRROR-CHECK" in out, "%.2fs" % dt, flush=True)

    dt, out = cmd(ser, "set N=0")
    lines = out.count("MIRROR")
    dt, out = cmd(ser, "for %v in (a b c d e f g h i j k l m n o p q r s t) do echo ITER-%v",
                  wait=1.0, timeout=60)
    iters = out.count("ITER-")
    print("for iters emitted: %d (expect 20)  %.2fs" % (iters, dt), flush=True)

    dt, out = cmd(ser, "for %v in (1 2 3 4 5 6 7 8 9 10) do set /a N+=%v",
                  wait=1.0, timeout=60)
    print("set/a loop done  %.2fs" % dt, flush=True)
    dt, out = cmd(ser, "echo N=%N%")
    print("N after loop:", "N=55" if "N=55" in out else out[-120:], flush=True)
    ser.close()


main()
