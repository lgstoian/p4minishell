#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""env_probe.py - count ambient env vars (Snake budget).

Usage: python env_probe.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["set", "echo EL=%ERRORLEVEL%"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-1200:], flush=True)
    sh.close()


main()
