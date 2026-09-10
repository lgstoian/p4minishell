#!/usr/bin/env python3
"""bg_kill4.py - mid-run cooperative kill with silence-drain timing.

Usage: python bg_kill4.py [COMx]
Expect: taskkill lands mid-run -> "stop requested" + "delay: stopped",
then slot reuse works.
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    t0 = time.time()
    sh = boot(port)
    print("booted in %.0fs" % (time.time() - t0), flush=True)
    for cmd in ["start delay 10000", "taskkill bg0", "delay 1500",
                "taskkill bg0", "start echo reuse-ok", "delay 2000"]:
        t = time.time()
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s (+%.1fs) ===" % (cmd, time.time() - t), flush=True)
        print(out[-500:], flush=True)
    sh.close()


main()
