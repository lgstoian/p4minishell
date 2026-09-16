#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tui_shot.py - draw then binary-capture screenshot in ONE session.

Usage: python tui_shot.py [COMx] [out.bmp]
Draws use plain writes (no marker echoes after the last draw, so nothing
disturbs TUI state), then captures the BMPX frame directly.
"""
import struct
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    out = sys.argv[2] if len(sys.argv) > 2 else "screenshots/tui_verify.bmp"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    draws = [
        "draw clear",
        "draw box 2 2 76 20 double 14 1 TUI Demo",
        "draw bar 5 6 40 65",
        'draw table 5 10 15 1 "Name|Score|Level" "Bob|1250|7" "Ada|980|5"',
        "tui status",
    ]
    for d in draws:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.5)
        chunk = sh.s.read(65536)
        print("--- %s (%d bytes)" % (d, len(chunk)), flush=True)
        tail = chunk.decode("utf-8", errors="replace")[-200:]
        print(tail, flush=True)
    # Screenshot without any follow-up transcript command.
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
                payload = data[i + 8:i + 8 + size]
                with open(out, "wb") as f:
                    f.write(payload)
                print("saved %s (%d bytes)" % (out, len(payload)), flush=True)
                break
    sh.close()


main()
