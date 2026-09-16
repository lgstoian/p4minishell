#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""audit_fixes_test.py - hardware checks for defects fixed in the v1.0.0 audit.

Covers behaviours the existing suites do not exercise:

  1. `plot auto` refits an inline `plot bar a,b,c` list. It previously tried to
     open the list as a file and failed with "cannot open".
  2. `del /s <file>` deletes same-named files in subdirectories even with the
     recycle bin enabled (the recursive pattern path used to be skipped).
  3. An overlong markdown pipe table renders every row (the row that overflowed
     `MD_TABLE_ROWS_MAX` used to be dropped).

Usage: python tools/audit_fixes_test.py COMx
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "apps", "companion"))
from shell_session import open_port          # noqa: E402
from push_sd import push_file, wait_shell     # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def drain(ser, seconds):
    end = time.time() + seconds
    out = b""
    while time.time() < end:
        d = ser.read(65536)
        if d:
            out += d
    return ANSI.sub("", out.decode("utf-8", "replace"))


def send(ser, line, wait=0.6):
    ser.write((line + "\r\n").encode())
    time.sleep(wait)


def run(port):
    ser = open_port(port, timeout=1)
    time.sleep(0.5)
    wait_shell(ser)
    ser.reset_input_buffer()
    fails = 0

    def check(name, ok):
        nonlocal fails
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
        if not ok:
            fails += 1

    # 1. plot auto with an inline bar list.
    ser.reset_input_buffer()
    send(ser, "gfx init 320 200", 0.8)
    send(ser, "plot bar 1,2,3", 0.8)
    send(ser, "plot auto", 0.8)
    out = drain(ser, 4)
    check("plot auto inline bar",
          "plot: window" in out and "cannot open" not in out)
    send(ser, "gfx close", 0.5)

    # 2. del /s recursion (recycle bin default). A unique tree per run so no
    #    destructive pre-clean (and its confirmation prompt) is needed.
    tag = "_AD%d" % (int(time.time()) % 100000)
    ser.reset_input_buffer()
    send(ser, "md " + tag, 0.4)
    send(ser, "md " + tag + "\\SUB", 0.4)
    send(ser, "write " + tag + "\\A.TXT one", 0.4)
    send(ser, "write " + tag + "\\SUB\\A.TXT two", 0.4)
    send(ser, "del /s " + tag + "\\A.TXT", 0.8)
    send(ser, "if exist " + tag + "\\SUB\\A.TXT echo STILL_THERE", 0.6)
    out = drain(ser, 4)
    check("del /s recurses into subdirs", "STILL_THERE" not in out)
    send(ser, "rd " + tag + "\\SUB", 0.3)
    send(ser, "rd " + tag, 0.3)

    # 3. Overlong markdown table keeps every row.
    name = "_AUDIT_TABLE.MD"
    body = ("| h1 | h2 |\n| --- | --- |\n" +
            "".join("| r%d | v%d |\n" % (i, i) for i in range(70)))
    wait_shell(ser)
    push_file(ser, name, body.encode())
    time.sleep(0.5)
    wait_shell(ser)
    ser.reset_input_buffer()
    send(ser, "markdown " + name, 1.0)
    out = drain(ser, 6)
    check("overlong markdown table row kept", "r69" in out)
    send(ser, "del /p " + name, 0.5)

    ser.close()
    if fails:
        print("RESULT FAIL (%d)" % fails)
        return 1
    print("RESULT OK (0 fail)")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1] if len(sys.argv) > 1 else "COM3"))
