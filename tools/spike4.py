#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""spike4.py - decisive space/tab choice test.

Usage: python spike4.py [COMx]
- /C with embedded real space: live-send space, expect selection.
- /C with embedded real tab: live-send tab, expect selection.
Reports raw repr.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike4_report.txt")


def read_all(ser, secs=4.0):
    end = time.time() + secs
    data = b""
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            end = time.time() + 1.0
    return data


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    ser = open_port(port)
    rep = []
    ser.reset_input_buffer()
    ser.write(b"choice /C:a b /N Pick?\r\n")
    time.sleep(2.0)
    ser.write(b" \r\n")
    rep.append("=== space in /C, live space ===")
    rep.append(repr(read_all(ser)))
    time.sleep(1.0)
    ser.reset_input_buffer()
    ser.write(b"choice /C:a\tb /N Pick?\r\n")
    time.sleep(2.0)
    ser.write(b"\t")
    rep.append("=== tab in /C, live tab ===")
    rep.append(repr(read_all(ser)))
    time.sleep(1.0)
    ser.reset_input_buffer()
    ser.write(b"choice /C:ab /N Pick?\r\n")
    time.sleep(2.0)
    ser.write(b"b\r\n")
    rep.append("=== baseline live b ===")
    rep.append(repr(read_all(ser)))
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
