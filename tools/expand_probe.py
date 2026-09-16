#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""expand_probe.py - does `call` double-expand %%vars%%?

Usage: python expand_probe.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["set s3x=42", "set k=3", "echo direct=%s3x%",
                "call echo via-call=%%s%k%x%%",
                "call set v=%%s%k%x%%", "echo v=%v%"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-250:], flush=True)
    sh.close()


main()
