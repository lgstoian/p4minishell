#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tui_close_test.py - is the grid in the TUI label or the transcript?

Usage: python tui_close_test.py [COMx]
Draws, screenshots, exits TUI (draw close), screenshots again.
"""
import struct
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def shot(sh, out):
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
                return
    print("screenshot TIMEOUT", flush=True)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    for d in ["draw clear",
              "draw box 2 2 40 10 single 14 1 Grid?",
              "tui status"]:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.5)
        print("--- %s: %s" % (d, sh.s.read(65536)[-120:]), flush=True)
    shot(sh, "screenshots/tui_open.bmp")
    sh.s.write(b"draw close\r\n")
    time.sleep(2.5)
    print("close: %s" % sh.s.read(65536)[-120:], flush=True)
    shot(sh, "screenshots/tui_closed.bmp")
    sh.close()


main()
