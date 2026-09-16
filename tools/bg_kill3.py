#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""bg_kill3.py - mid-run cooperative kill (the real test).

Usage: python bg_kill3.py [COMx]
start delay 10000 (clamp max) -> taskkill lands ~2 s in:
expect "stop requested" then "delay: stopped", slot reusable after.
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.reset_input_buffer()
    # Let boot wifi chatter finish first, or it eats the kill window.
    sh.run("delay 12000", timeout=25)
    for cmd in ["start delay 10000", "taskkill bg0", "delay 1500",
                "taskkill bg0", "start echo reuse-ok", "delay 1500"]:
        try:
            out = sh.run(cmd, timeout=25)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-500:], flush=True)
    sh.close()


main()
