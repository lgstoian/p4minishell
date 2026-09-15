#!/usr/bin/env python3
"""bind_test.py - hardware driver for the F-key bind table.

Checks bind set/list, F-key range rejection, /save + /clear + /load
round-trip through a profile file, then cleans up. Prints RESULT OK.
Usage: python tools/bind_test.py COMx

Note: live F-key firing needs a physical USB keyboard (HID F1..F12);
the lookup path itself is covered by the unit suite (test_bind.c).
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from shell_session import open_port


def run(port):
    ser = open_port(port, timeout=10)
    time.sleep(1)
    ser.reset_input_buffer()

    lines = [
        "bind F5 echo bound-ok",
        "bind F12 sysinfo",
        "bind",
        "bind F13 echo nope",
        "bind unbind F12",
        "bind",
        "bind /save _bind_hw.bat",
        "type _bind_hw.bat",
        "bind /clear",
        "bind",
        "bind /load _bind_hw.bat",
        "bind",
        "del _bind_hw.bat",
        "bind /clear",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(0.4)

    deadline = time.time() + 40
    out = ""
    while time.time() < deadline:
        chunk = ser.read(65536).decode("utf-8", "replace")
        out += chunk
        if out.count("all binds cleared") >= 2:
            break
        if len(chunk) == 0:
            time.sleep(0.5)
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)

    assert "bind: F5 set" in out, "set failed:\n" + out
    assert "F5=echo bound-ok" in out, "list failed:\n" + out
    assert "F12=sysinfo" in out, "second bind failed:\n" + out
    assert "Usage:" in out, "F13 should print usage:\n" + out
    assert "bind F5 echo bound-ok" in out, "profile content failed:\n" + out
    assert "loaded 1 bind(s)" in out, "load failed:\n" + out
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
