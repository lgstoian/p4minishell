#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""bg_test.py - drive start/taskkill scenarios on-board.

Usage: python bg_test.py [COMx]
Connect pulses DTR (board reboots), so we settle, then run each case.
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell

CMDS = [
    "echo READY",
    "start echo hello-from-bg",
    "delay 1500",
    "taskkill bg0",
    "taskkill bg9",
    "start",
    "help start",
]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)  # boot (AUTOEXEC + wifi)
    sh.s.reset_input_buffer()
    for cmd in CMDS:
        out = sh.run(cmd, timeout=25)
        print("=== %s ===" % cmd)
        print(out[-900:])
        print()
    sh.close()


main()
