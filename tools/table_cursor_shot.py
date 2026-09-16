#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""table_cursor_shot.py - Phase 2 HW verify: /cursor + /sel rendering.

Usage: python table_cursor_shot.py [COMx]
One Shell session: settle boot, draw a 3-col table with cursor row 2 +
sel 1,3, screenshot in-session (no port reopen), then usage-error checks
with errorlevels. Saves spikes/table_cursor.bmp.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import Shell

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "spikes", "table_cursor.bmp")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    draws = [
        "draw hold on",
        'draw table 2 4 7 0 "Name|Size|Date" "ADVENT.BAT|6.0 KiB|1980" "NOTES.BAT|3.3 KiB|1980" "MOOD.BAT|2.3 KiB|1980" /cursor:2 /sel:1,3',
        "draw refresh",
    ]
    for d in draws:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.5)
        chunk = sh.s.read(65536)
        print("--- %s (%d bytes)" % (d, len(chunk)), flush=True)
        print(chunk.decode("utf-8", errors="replace")[-200:], flush=True)
    sh.s.reset_input_buffer()
    sh.s.write(b"screenshot\r\n")
    data = b""
    end = time.time() + 60
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"BMPX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                print("frame: size=%d" % size, flush=True)
                while len(data) < i + 8 + size and time.time() < end:
                    more = sh.s.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open(OUT, "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                print("saved %s" % OUT, flush=True)
                break
    for d in ['draw table 2 4 7 0 /cursor:2',
              'draw table 2 4 7 0 "A|B" "x|y" /cursor:9',
              'draw table 2 4 7 0 "A|B" "x|y" /sel:1,99',
              "echo EL=%ERRORLEVEL%",
              "draw hold off",
              "draw close"]:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.5)
        chunk = sh.s.read(65536)
        print("--- %s" % d, flush=True)
        print(chunk.decode("utf-8", errors="replace")[-300:], flush=True)
    sh.close()


main()
