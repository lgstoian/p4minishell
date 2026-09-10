#!/usr/bin/env python3
"""launch_check.py - why do ADVENT/NOTES/MOOD fail to launch?

Usage: python launch_check.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["dir ADVENT.BAT", "dir NOTES.BAT", "dir MOOD.BAT",
                "launch /list", "ADVENT.BAT"]:
        try:
            out = run_quiet(sh, cmd, timeout=40)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-800:], flush=True)
    sh.close()


main()
