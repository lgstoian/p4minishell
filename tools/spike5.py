#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""spike5.py - decisive space/+/-/Enter choice test, one stable session.

Usage: python spike5.py [COMx]
Opens serial once, waits out boot, then:
1. /C:ab+c live '+' delivery
2. /C:ab- live '-' delivery
3. /C:ab<space>c live ' ' delivery
4. /C:ab live Enter (bare \r) delivery
Each reports raw repr; selection echo proves delivery.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike5_report.txt")


def read_all(ser, idle=1.5):
    end = time.time() + idle
    data = b""
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            end = time.time() + 0.7
    return data


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    ser = open_port(port)
    time.sleep(12)  # ride out the DTR reboot + boot spam
    read_all(ser)
    rep = []
    cases = [
        ("plus", b"choice /C:ab+c /N Pick?\r\n", b"+"),
        ("minus", b"choice /C:ab-c /N Pick?\r\n", b"-"),
        ("space", b"choice /C:ab c /N Pick?\r\n", b" "),
        ("enter", b"choice /C:ab /N Pick?\r\n", b"\r"),
    ]
    for name, setup, key in cases:
        ser.reset_input_buffer()
        ser.write(setup)
        time.sleep(2.0)
        ser.write(key)
        out = read_all(ser)
        rep.append("=== %s (sent %r) ===" % (name, key))
        rep.append(repr(out[-400:]))
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
