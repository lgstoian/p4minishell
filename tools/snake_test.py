#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""snake_test.py - run SNAKE.BAT straight into the wall (no steering).

Usage: python snake_test.py [COMx]
Auto-plays via choice timeouts: head moves right from x=20 to the wall.
Expects M-SNAKE, frames, M-SNAKE-DONE + SNAKE.BMP screenshot on SD.
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
    sh.s.timeout = 0.5
    end = time.time() + 180
    data = b""
    done = False
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            for p in (b"Guru Meditation", b"Stack protection", b"Backtrace",
                      b"assert failed", b"abort()"):
                if p in data:
                    print("PANIC: %r" % p, flush=True)
                    print(data[-1500:].decode("utf-8", errors="replace"), flush=True)
                    sh.close()
                    return
            if b"M-SNAKE-DONE" in data:
                done = True
                break
    text = data.decode("utf-8", errors="replace")
    print("done=%s eats=%d" % (done, text.count("M-SNAKE-EAT")), flush=True)
    i = text.find("M-SNAKE-DONE")
    if i >= 0:
        print(text[max(0, i - 120):i + 50][-200:], flush=True)
    else:
        print("tail:", flush=True)
        print(text[-600:], flush=True)
    sh.close()


main()
