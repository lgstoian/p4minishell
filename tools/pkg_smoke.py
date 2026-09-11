#!/usr/bin/env python3
"""pkg_smoke.py - quick pkg list/info/verify against existing manifests."""
import sys

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    for cmd in ["pkg list", "pkg info BOUNCE", "pkg verify BOUNCE",
                "pkg verify NOPE", "pkg info bad/name", "pkg bogus"]:
        out = run_quiet(sh, cmd)
        print("=== %s ===" % cmd)
        print(out[-700:])
    sh.close()


main()
