#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""assets_verify.py - asset check every pushed manifest + sprite load.

Usage: python assets_verify.py [COMx]
Runs `asset check` for each app manifest and loads both sprites.
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    ok = True
    for app in ["SPR", "BOUNCE", "SNAKE", "TCMD", "ELITE", "ADVENT",
                "NOTES", "MOOD"]:
        try:
            out = run_quiet(sh, "asset check " + app)
        except RuntimeError as e:
            print("PANIC after asset check %s: %s" % (app, e), flush=True)
            ok = False
            break
        good = ("asset: OK" in out)
        print("%-8s %s" % (app, "OK" if good else "FAIL"), flush=True)
        if not good:
            print(out[-300:], flush=True)
            ok = False
    for cmd in ["gfx init 160 120", "gfx load 0 SHIP.BMP",
                "gfx load 1 BALL.BMP", "gfx slots",
                "gfx blit 0 20 20", "gfx blit 1 100 80 0",
                "gfx show", "gfx close"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            ok = False
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-250:], flush=True)
    print("RESULT %s" % ("OK" if ok else "FAIL"), flush=True)
    sh.close()


main()
