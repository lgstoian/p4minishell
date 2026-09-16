#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""snake_steer_test.py - spam keys to hit armed choice windows.

Usage: python snake_steer_test.py [COMx]
Spams 's' (turn down) then 'q' (quit). QUIT suffix proves live delivery;
survival past the straight-line wall time implies the turn landed.
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    sh.s.reset_input_buffer()
    sh.s.write(b"SNAKE\r\n")
    # Serial keys need Enter (UART reader is line-buffered): spam 's\n'
    # then 'q\n'. Each line either lands in an armed choice wait (steers)
    # or queues one harmless unknown command behind the running game.
    end = time.time() + 10
    while time.time() < end:
        sh.s.write(b"s\n")
        time.sleep(1.5)
    end = time.time() + 15
    while time.time() < end:
        sh.s.write(b"q\n")
        time.sleep(1.5)
    sh.s.timeout = 0.5
    end = time.time() + 100
    data = b""
    done = False
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            if b"M-SNAKE-DONE" in data:
                done = True
                break
    text = data.decode("utf-8", errors="replace")
    print("done=%s" % done, flush=True)
    i = text.find("M-SNAKE-DONE")
    print(text[max(0, i - 100):i + 50][-160:] if i >= 0 else text[-400:], flush=True)
    sh.close()


main()
