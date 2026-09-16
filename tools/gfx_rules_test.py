#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gfx_rules_test.py - bg refusal + launch discovery for gfx/BOUNCE.

Usage: python gfx_rules_test.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["launch /list", "start BOUNCE", "delay 1500",
                "taskkill bg0", "gfx init 400 300", "gfx init 0 10",
                "gfx close", "gfx pixel 1 1 7"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-450:], flush=True)
    sh.close()


main()
