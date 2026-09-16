#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""spike3.py - Phase 1 follow-ups, corrected.

Usage: python spike3.py [COMx]
1. dir > _spkd.txt, pull via pull.py, dump raw repr (ANSI in file?).
2. for /f tokens=1,2,3,* syntax.
3. Real TAB (0x09) in choice /C + live delivery; space with long wait.
4. Verify junk cleanup + report SD root.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike3_report.txt")
PULL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "spikes", "_spkd_pulled.txt")


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
    rep = []
    cmd(ser, "dir > _spkd.txt", wait=4.0)
    ser.close()
    r = subprocess.run([sys.executable, "tools/pull.py", "_spkd.txt", PULL, port],
                       capture_output=True, text=True, timeout=180,
                       cwd=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    rep.append("=== pull.py ===")
    rep.append(r.stdout[-300:] + r.stderr[-300:])
    if os.path.exists(PULL):
        with open(PULL, "rb") as f:
            raw = f.read()
        rep.append("=== _spkd.txt size=%d first 700 bytes repr ===" % len(raw))
        rep.append(repr(raw[:700]))
    else:
        rep.append("PULL FILE MISSING")
    ser = open_port(port)
    rep.append("=== tokens=1,2,3,* ===")
    rep.append(repr(cmd(ser, "for /f \"tokens=1,2,3,*\" %%a in (_spkd.txt) do echo A=[%%a] B=[%%b] C=[%%c] D=[%%d]")[-900:]))
    rep.append("=== real TAB in /C (timeout path) ===")
    rep.append(repr(cmd(ser, "choice /C:ab\t /N /T:a,2 Pick?", wait=6.0)[-300:]))
    rep.append("=== live TAB delivery ===")
    ser.reset_input_buffer()
    ser.write(b"choice /C:ab /N Pick?\r\n")
    time.sleep(2.0)
    ser.write(b"\t")
    time.sleep(4.0)
    data = b""
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
        data += chunk
    rep.append(repr(data[-400:]))
    rep.append("=== live SPACE delivery (long wait) ===")
    ser.reset_input_buffer()
    ser.write(b"choice /C:ab /N Pick?\r\n")
    time.sleep(2.0)
    ser.write(b" ")
    time.sleep(5.0)
    data = b""
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
        data += chunk
    rep.append(repr(data[-400:]))
    rep.append("=== cleanup verify ===")
    rep.append(repr(cmd(ser, "dir /b")[-1200:]))
    for f in ["del /p _spkd.txt", "del /p _spk.txt", "del /p _T.txt"]:
        rep.append("%s -> %s" % (f, repr(cmd(ser, f)[-150:])))
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
