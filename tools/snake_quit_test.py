#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""snake_quit_test.py - prove live keys reach SNAKE (quit path).

Usage: python snake_quit_test.py [COMx]
Launches SNAKE, waits 4 s (a few frames), sends 'q', expects QUIT DONE.
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
    time.sleep(4)
    # Steering keys + quit: choice reads single keys.
    sh.s.write(b"s")
    time.sleep(3)
    sh.s.write(b"q")
    sh.s.timeout = 0.5
    end = time.time() + 120
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
    print(text[max(0, i - 100):i + 40][-160:] if i >= 0 else text[-400:], flush=True)
    sh.close()


main()
