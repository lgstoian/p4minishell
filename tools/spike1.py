#!/usr/bin/env python3
"""spike1.py - Phase 1 HW spike: raw dir bytes (ANSI?), skip= paging, keys.

Usage: python spike1.py [COMx]  (default COM11)
Writes results to spikes/spike1_report.txt (raw repr, no ANSI stripping).
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike1_report.txt")


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
    rep.append("=== dir /b (raw) ===")
    rep.append(repr(cmd(ser, "dir /b")[-1500:]))
    rep.append("=== dir (detailed, raw) ===")
    rep.append(repr(cmd(ser, "dir")[-3000:]))
    rep.append("=== dir /o:n (raw tail) ===")
    rep.append(repr(cmd(ser, "dir /o:n")[-800:]))
    # for /f skip= paging + token parse on a captured listing
    rep.append("=== capture + for/f skip/tokens ===")
    cmd(ser, "dir /b > _spk.txt")
    rep.append(repr(cmd(ser, "for /f \"skip=2 tokens=1\" %%a in (_spk.txt) do echo ROW=[%%a]")[-1500:]))
    rep.append("=== choice key map probe (send j, k, space, tab, digits) ===")
    rep.append(repr(cmd(ser, "choice /C:jk123 /N /T:x,2 Key?", wait=6.0)[-800:]))
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
