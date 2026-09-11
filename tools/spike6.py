#!/usr/bin/env python3
"""spike6.py - render-pipeline primitives go/no-go.

Usage: python spike6.py [COMx]
1. dir sd:/APPS (sd: prefix vs switch-parse?)
2. for /f "delims=" whole-line
3. set-append accumulation across iterations
4. usebackq string + tokens=1-6 range split
5. assembled draw table from vars (6-row page render!)
Saves spikes/table_vars.bmp screenshot.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "spike6_report.txt")
SPIKES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "spikes")


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
    rep.append("=== 1. dir sd:/APPS ===")
    rep.append(repr(cmd(ser, "dir sd:/APPS")[-500:]))
    rep.append("=== 2. delims= whole line ===")
    cmd(ser, "echo alpha beta gamma > _w.txt")
    rep.append(repr(cmd(ser, 'for /f "delims=" %%a in (_w.txt) do echo WHOLE=[%%a]')[-300:]))
    rep.append("=== 3. set-append ===")
    cmd(ser, "set ACC=")
    cmd(ser, "echo one > _a.txt")
    cmd(ser, "echo two >> _a.txt")
    cmd(ser, "echo three >> _a.txt")
    rep.append(repr(cmd(ser, "for /f %%a in (_a.txt) do set ACC=%ACC%%%a|")[-300:]))
    rep.append(repr(cmd(ser, "echo ACC=%ACC%")[-200:]))
    rep.append("=== 4. usebackq string + tokens=1-6 ===")
    rep.append(repr(cmd(ser, 'for /f "usebackq tokens=1-6 delims=|" %%a in ("r1|r2|r3|r4|r5|r6") do echo T=[%%a][%%b][%%c][%%d][%%e][%%f]')[-300:]))
    rep.append("=== 5. var-driven draw table ===")
    cmd(ser, "draw hold on")
    cmd(ser, 'set R1=ADVENT.BAT|6.0 KiB|1980-01-01')
    cmd(ser, 'set R2=NOTES.BAT|3.3 KiB|1980-01-01')
    out = cmd(ser, 'draw table 2 4 7 0 "Name|Size|Date" "%R1%" "%R2%" /cursor:1')
    rep.append(repr(out[-300:]))
    cmd(ser, "draw refresh")
    time.sleep(1.5)
    # screenshot in-session
    ser.reset_input_buffer()
    ser.write(b"screenshot\r\n")
    data = b""
    end = time.time() + 60
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"BMPX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                while len(data) < i + 8 + size and time.time() < end:
                    more = ser.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open(os.path.join(SPIKES, "table_vars.bmp"), "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                rep.append("shot saved")
                break
    rep.append("=== cleanup ===")
    for f in ["del /p _w.txt", "del /p _a.txt"]:
        rep.append(repr(cmd(ser, f)[-150:]))
    cmd(ser, "draw hold off")
    cmd(ser, "draw close")
    ser.close()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(rep))
    print("wrote", OUT)


main()
