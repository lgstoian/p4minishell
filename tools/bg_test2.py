#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""bg_test2.py - start/taskkill lifecycle on-board.

Usage: python bg_test2.py [COMx]
1. long delay in bg -> taskkill mid-run (cooperative stop)
2. slot reuse after kill
3. batch file in bg (echo/delay loop, killed mid-run)
4. double-start while busy (pool exhausted)
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

    cases = [
        "start delay 8000",
        "delay 500",
        "taskkill bg0",
        "delay 500",
        "start echo reuse-ok",
        "delay 1200",
        "echo begin > BGTEST.BAT",
        "echo echo bg-batch-line >> BGTEST.BAT",
        "echo delay 2000 >> BGTEST.BAT",
        "echo echo bg-batch-done >> BGTEST.BAT",
        "type BGTEST.BAT",
        "start BGTEST",
        "delay 500",
        "taskkill bg0",
        "delay 2500",
        "del BGTEST.BAT",
    ]
    for cmd in cases:
        out = sh.run(cmd, timeout=25)
        print("=== %s ===" % cmd)
        print(out[-700:])
        print()
    sh.close()


main()
