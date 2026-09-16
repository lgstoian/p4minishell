# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Transcript drain benchmark for bugs.md O3 (O(buffer) span-rebuild cost).

Streams a large SD file twice back-to-back and reports cumulative drain
rates. A healthy console drains at line rate (~11 KB/s @115200) regardless
of buffer state. The O(buffer) pathology shows as:
  - run 1 (empty buffer): fast start (~3-5 KB/s), decaying as the 64 KB
    transcript buffer fills,
  - run 2 (buffer full): slow from the first byte (~1 KB/s or less).

Mechanism: every shell_transcript_append_internal() calls
shell_transcript_update_label() -> windows_set_transcript_text(), which
reparses the whole ANSI buffer and rebuilds all spans + layout per append.
The async background path coalesces; the worker path does not.

Usage: python transcript_drain_bench.py [COMx]
Requires sd:/esp32c6_hosted_slave.bin (or any multi-hundred-KB file: pass
its path as argv[2]).
"""

import re
import sys
import time
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

PORT = default_port()
BIGFILE = sys.argv[2] if len(sys.argv) > 2 else "esp32c6_hosted_slave.bin"


def drain_once(ser, run_no, marks=(10000, 30000, 60000)):
    ser.reset_input_buffer()
    ser.write(("type %s\n" % BIGFILE).encode())
    t0 = time.time()
    n = 0
    mi = 0
    marks = list(marks)
    while time.time() - t0 < 150 and mi < len(marks):
        d = ser.read(65536)
        if d:
            n += len(d)
            while mi < len(marks) and n >= marks[mi]:
                dt = time.time() - t0
                print("run %d cum %6d at t+%5.1fs (rate %.1f KB/s)" % (
                    run_no, marks[mi], dt, marks[mi] / 1024 / dt), flush=True)
                mi += 1
        else:
            time.sleep(0.1)


def main():
    ser = open_port(PORT, 115200, timeout=1)
    time.sleep(15.0)  # boot quiesce
    ser.reset_input_buffer()
    print("== run 1 (empty buffer; send cls first for a clean baseline) ==")
    ser.write(b"cls\n")
    time.sleep(3.0)
    ser.read(ser.in_waiting or 1)
    drain_once(ser, 1)
    time.sleep(2.0)
    print("== run 2 (buffer full) ==")
    drain_once(ser, 2)
    ser.close()


if __name__ == "__main__":
    main()
