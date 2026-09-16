#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""userial_test.py - hardware driver for the `usb userial` CDC-ACM verbs.

Exercises the no-device paths (status/open-timeout/close/recv/usage) and
checks the `usb` family still reports a ready host. A real CDC device is
optional: set P4_USERIAL_ID=vvvv:pppp to also run open/status/close against
attached hardware.
Prints RESULT OK on success.
Usage: python tools/userial_test.py COMx
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_session import open_port


def run(port):
    ser = open_port(port, timeout=15)
    time.sleep(1)
    ser.reset_input_buffer()

    lines = [
        "usb status",
        "usb userial status",
        "usb userial open 1234:5678",
        "usb userial close",
        "usb userial recv",
        "usb userial bogus",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(1.0)

    deadline = time.time() + 60
    out = ""
    while time.time() < deadline:
        chunk = ser.read(65536).decode("utf-8", "replace")
        out += chunk
        if "Usage: usb userial" in out and "no device open" in out:
            break
        if len(chunk) == 0:
            time.sleep(0.5)
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)

    assert "usb.host: ready" in out, "usb family regressed:\n" + out
    assert "userial: no device open" in out, "status path failed:\n" + out
    assert "no matching device" in out, "open-timeout path failed:\n" + out
    assert "Usage: usb userial" in out, "usage path failed:\n" + out

    dev = os.environ.get("P4_USERIAL_ID")
    if dev:
        ser.write(("usb userial open " + dev + "\r\n").encode())
        time.sleep(2.0)
        ser.write(b"usb userial status\r\n")
        time.sleep(1.0)
        ser.write(b"usb userial close\r\n")
        time.sleep(1.0)
        reply = ser.read(65536).decode("utf-8", "replace")
        reply = re.sub(r"\x1b\[[0-9;]*m", "", reply)
        assert "userial.device" in reply, "device open failed:\n" + reply
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
