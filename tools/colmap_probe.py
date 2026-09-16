#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""colmap_probe.py - measure TUI col->pixel mapping.

Usage: python colmap_probe.py [COMx]
Draws markers at cols 1/40/48/80, screenshots, measures bright pixels.
"""
import struct
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    for d in ["draw clear", "draw text 1 10 A", "draw text 40 10 B",
              "draw text 48 10 C", "draw text 80 10 D"]:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.0)
        sh.s.read(65536)
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
                while len(data) < i + 8 + size and time.time() < end:
                    more = sh.s.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open("screenshots/colmap.bmp", "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                print("saved", flush=True)
                break
    sh.s.write(b"draw close\r\n")
    time.sleep(1)
    sh.close()


main()
