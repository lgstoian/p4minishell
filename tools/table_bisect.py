#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""table_bisect.py - isolate the dropped-column cause.

Usage: python table_bisect.py [COMx]
Shot A: 3-col table, NO flags. Shot B: same + /cursor + /sel.
Saves spikes/table_A.bmp, spikes/table_B.bmp.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import Shell

HERE = os.path.dirname(os.path.abspath(__file__))
SPIKES = os.path.join(HERE, "..", "spikes")


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


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    sh = Shell(port)
    time.sleep(12)
    sh.s.timeout = 1
    sh.s.reset_input_buffer()
    for d in ["draw hold on",
              'draw table 2 4 7 0 "A|B|C" "1|2|3" "4|5|6"',
              "draw refresh"]:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.5)
        print("--- %s" % d, flush=True)
        print(sh.s.read(65536).decode("utf-8", errors="replace")[-200:], flush=True)
    shot(sh, os.path.join(SPIKES, "table_A.bmp"))
    for d in ['draw table 2 12 7 0 "A|B|C" "1|2|3" "4|5|6" /cursor:2 /sel:1',
              "draw refresh"]:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.5)
        print("--- %s" % d, flush=True)
        print(sh.s.read(65536).decode("utf-8", errors="replace")[-200:], flush=True)
    shot(sh, os.path.join(SPIKES, "table_B.bmp"))
    for d in ["draw hold off", "draw close"]:
        sh.s.write((d + "\r\n").encode())
        time.sleep(2.0)
        sh.s.read(65536)
    sh.close()


main()
