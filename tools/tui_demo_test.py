#!/usr/bin/env python3
"""tui_demo_test.py - draw bar/table/color demo + screenshot.

Usage: python tui_demo_test.py [COMx]
"""
import os
import sys
import time

sys.path.insert(0, "tools")
sys.path.insert(0, os.path.join("tools", "harness"))
from bg_run import run_quiet, boot
from grab_screenshot import grab_screenshot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    cmds = [
        "draw clear",
        "draw box 2 2 76 20 double 14 1 TUI Demo",
        "draw text 5 4 Hi 15 1",
        "draw bar 5 6 40 65",
        "draw bar 5 8 40 30 # . 10 1",
        'draw table 5 10 15 1 "Name|Score|Level" "Bob|1250|7" "Ada|980|5"',
        "draw bar 5 6 40 90",
    ]
    for cmd in cmds:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-300:], flush=True)
    time.sleep(2)
    sh.close()
    shot = grab_screenshot(port=port, out_dir="screenshots")
    print("screenshot: %s" % shot, flush=True)


main()
