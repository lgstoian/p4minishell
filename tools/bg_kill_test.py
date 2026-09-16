#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""bg_kill_test.py - minimal cooperative-kill bisection.

Usage: python bg_kill_test.py [COMx]
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
    for cmd in ["echo READY", "start delay 8000", "delay 500", "taskkill bg0",
                "delay 1000", "echo AFTER-KILL", "ps"]:
        try:
            out = sh.run(cmd, timeout=25)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-700:], flush=True)
    sh.close()


main()
