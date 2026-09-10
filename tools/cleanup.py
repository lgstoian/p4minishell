#!/usr/bin/env python3
"""cleanup.py - remove test litter from the SD root.

Usage: python cleanup.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["del VA.BAT", "del VB.BAT", "del VC.BAT", "del BGTEST.BAT",
                "del BOUNCE.BMP", "del SPR32.BMP", "del SPR_OUT.BMP",
                "del SNAKE.BMP", "del ELITE.BMP", "del ELITE.SAV",
                "del APPS/TESTA.ASSETS", "dir"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-500:], flush=True)
    sh.close()


main()
