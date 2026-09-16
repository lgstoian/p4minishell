#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tui_diag.py - which draw path is taken + errorlevels.

Usage: python tui_diag.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["tui status", "draw clear", "echo EL=%ERRORLEVEL%",
                "tui status", "draw box 2 2 20 6 single 14 1 Hi",
                "echo EL=%ERRORLEVEL%", "tui status"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-400:], flush=True)
    sh.close()


main()
