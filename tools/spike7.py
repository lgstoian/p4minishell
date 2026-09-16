#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""spike7.py - ^| escape + findstr invert + pipeline-to-file go/no-go.

Usage: python spike7.py [COMx]
1. echo a^|b (literal pipe in value?)
2. for-body with ^| join into file, then type it
3. findstr /v invert on dir output, piped to file
4. dir | findstr composition
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike7_report.txt")


def cmd(ser, text, wait=3.0):
    ser.reset_input_buffer()
    ser.write((text + "\r\n").encode())
    time.sleep(wait)
    data = b""
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
        data += chunk
    return data


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    ser = open_port(port)
    time.sleep(12)
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
    rep = []
    rep.append("=== 1. echo caret-pipe ===")
    rep.append(repr(cmd(ser, "echo a^|b^|c")[-200:]))
    rep.append("=== 2. for-body join ===")
    cmd(ser, "echo one > _j.txt")
    cmd(ser, "echo two >> _j.txt")
    rep.append(repr(cmd(ser, "for /f %%a in (_j.txt) do echo %%a^|%%a >> _k.txt")[-300:]))
    rep.append(repr(cmd(ser, "type _k.txt")[-300:]))
    rep.append("=== 3. findstr /v ===")
    rep.append(repr(cmd(ser, "findstr /v file _k.txt")[-300:]))
    rep.append("=== 4. dir | findstr > file ===")
    rep.append(repr(cmd(ser, "dir /b | findstr /v BAT > _nb.txt")[-300:]))
    rep.append(repr(cmd(ser, "type _nb.txt")[-400:]))
    rep.append("=== 5. findstr help ===")
    rep.append(repr(cmd(ser, "findstr /?")[-600:]))
    rep.append("=== cleanup ===")
    for f in ["del /p _j.txt", "del /p _k.txt", "del /p _nb.txt"]:
        rep.append(repr(cmd(ser, f)[-120:]))
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
