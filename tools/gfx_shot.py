#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gfx_shot.py - gfx canvas demo + binary screenshot in one session.

Usage: python gfx_shot.py [COMx] [out.bmp]
"""
import struct
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    out = sys.argv[2] if len(sys.argv) > 2 else "screenshots/gfx_demo.bmp"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    draws = [
        "gfx status",
        "gfx init 240 180",
        "gfx clear 1",
        "gfx rect 10 10 100 60 14",
        "gfx rect 20 20 60 30 12 fill",
        "gfx circle 180 90 40 10",
        "gfx circle 180 90 25 11 fill",
        "gfx line 0 0 239 179 15",
        "gfx pixel 5 5 0xFFAA00",
        "gfx show",
        "gfx status",
        "gfx rect 0 0 500 500 4",
        "gfx show",
    ]
    for d in draws:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.0)
        chunk = sh.s.read(65536)
        print("--- %s" % d, flush=True)
        print(chunk.decode("utf-8", errors="replace")[-250:], flush=True)
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
                with open(out, "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                print("saved %s" % out, flush=True)
                break
    sh.close()


main()
