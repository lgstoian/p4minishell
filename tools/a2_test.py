#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""a2_test.py - HW verify A2: bg refusal + hold coalescing.

Usage: python a2_test.py [COMx]
`start <verb>` runs the verb itself in the bg worker: refusal text lands
in the transcript asynchronously and the marker echo proves ordering.
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def el(sh, cmd):
    out = run_quiet(sh, cmd)
    print("=== %s ===" % cmd, flush=True)
    print(out[-400:], flush=True)
    return out


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    # Foreground hold behavior + errorlevels.
    for cmd in ["draw hold on", "echo EL=%ERRORLEVEL%",
                "draw box 2 2 20 6 single Hi", "draw hold maybe",
                "echo EL=%ERRORLEVEL%", "draw hold off",
                "echo EL=%ERRORLEVEL%", "draw close"]:
        el(sh, cmd)
    # Background refusal, one verb per `start` (direct bg execution).
    for cmd in ["start draw box 2 2 10 4 single BG",
                "start tui clear",
                "start tui status",
                "start color 14 1",
                "start color",
                "start locate 5 5",
                "start anchor L1 echo-hi",
                "start gfx clear 1"]:
        el(sh, cmd)
    el(sh, "delay 3000")
    el(sh, "taskkill bg0")
    sh.close()


main()
