#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""spike2.py - Phase 1 follow-up: redirect ANSI?, token parse, space/tab keys.

Usage: python spike2.py [COMx]
1. dir > _spkd.txt, then pull it via send (SDFX) and dump raw repr.
2. for /f tokens parse of detailed rows (adapts after seeing #1).
3. choice /C with space + tab: timeout-default path + live keypress delivery.
4. Cleanup junk files from earlier rem bugs (` files, TUI file, _spk/_T).
"""
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike2_report.txt")


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


def pull(ser, path):
    """send <path> -> SDFX frame -> payload bytes."""
    ser.reset_input_buffer()
    ser.write(("send %s\r\n" % path).encode())
    data = b""
    end = time.time() + 30
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"SDFX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                while len(data) < i + 8 + size and time.time() < end:
                    more = ser.read(65536)
                    if more:
                        data += more
                return data[i + 8:i + 8 + size]
    return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    ser = open_port(port)
    rep = []
    cmd(ser, "dir /b > _spkd.txt")
    payload = pull(ser, "_spkd.txt")
    rep.append("=== _spkd.txt raw bytes (first 600) ===")
    rep.append(repr(payload[:600]) if payload else "PULL FAILED")
    # token parse attempt on detailed rows via type (visual) + for/f
    rep.append("=== for/f tokens=1,2,3* on detailed capture ===")
    cmd(ser, "dir > _spkd.txt")
    out = cmd(ser, "for /f \"tokens=1,2,3*\" %%a in (_spkd.txt) do echo A=[%%a] B=[%%b] C=[%%c]")
    rep.append(repr(out[-1200:]))
    rep.append("=== choice with space+tab in /C (timeout path) ===")
    out = cmd(ser, "choice /C:ab\\t /N /T:a,2 Pick?", wait=6.0)
    rep.append(repr(out[-400:]))
    rep.append("=== choice live spacebar delivery ===")
    ser.reset_input_buffer()
    ser.write(b"choice /C:ab /N Pick?\r\n")
    time.sleep(2.0)
    ser.write(b" \r\n")
    time.sleep(2.0)
    data = b""
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
        data += chunk
    rep.append(repr(data[-500:]))
    rep.append("=== cleanup junk ===")
    for f in ["_spk.txt", "_spkd.txt", "_T.txt"]:
        rep.append("%s: %s" % (f, repr(cmd(ser, "del /p %s" % f)[-200:])))
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
