#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""capture_docs.py - curated public screenshots for docs/assets/.

Drives the board over USB serial into a fixed list of states and captures
each screen with the streaming `screenshot` protocol, saving PNG only
(no BMP clutter) to docs/assets/<name>.png.

All states are non-destructive: no persistence writes (no /save, no SD
writes, editor quits without saving), and the theme is restored to
`default` at the end.

Usage: python tools/capture_docs.py [COMx]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p4test.device import Device  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "docs", "assets")


def sh(dev, cmd, sleep_s=1.5):
    """Send a command without harness markers (keeps shots authentic)."""
    dev.send(cmd)
    time.sleep(sleep_s)
    dev.read_for(0.5)


def shot(dev, name):
    bmp = dev.screenshot()
    path = os.path.join(OUT, name + ".png")
    bmp.save_png(path)
    print("  saved %s (%dx%d)" % (path, bmp.width, bmp.height))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    os.makedirs(OUT, exist_ok=True)
    fails = []
    with Device(port, boot=True, settle_ms=4000) as dev:
        try:
            print("== shell-idle ==")
            sh(dev, "cls")
            shot(dev, "shell-idle")

            print("== shell-help ==")
            sh(dev, "cls")
            sh(dev, "help", 2.0)
            shot(dev, "shell-help")

            print("== shell-dir ==")
            sh(dev, "cls")
            sh(dev, "dir", 2.0)
            shot(dev, "shell-dir")

            print("== apps-launch ==")
            sh(dev, "cls")
            sh(dev, "launch /list", 2.5)
            shot(dev, "apps-launch")

            # --- modal: dialog (auto-cancels via /t) ---
            print("== modal-dialog ==")
            sh(dev, "cls")
            dev.send('dialog /t:10 "Delete file?" "Remove NOTES.TXT permanently?" Yes No')
            time.sleep(2.5)
            shot(dev, "modal-dialog")
            time.sleep(9.0)
            dev.read_for(2.0)

            # --- modal: list ---
            print("== modal-list ==")
            sh(dev, "cls")
            dev.send('list /t:10 "Pick a colour" Red Green Blue Amber')
            time.sleep(2.5)
            shot(dev, "modal-list")
            time.sleep(9.0)
            dev.read_for(2.0)

            # --- editor (quit without saving) ---
            print("== editor-editing ==")
            sh(dev, "cls")
            dev.send("edit CONFIG.SYS")
            time.sleep(3.0)
            shot(dev, "editor-editing")
            dev.send("\\q")
            time.sleep(1.5)
            dev.read_for(2.0)

            # --- TUI boxes ---
            print("== tui-boxes ==")
            sh(dev, "draw clear screen")
            sh(dev, 'draw box 2 2 36 10 single "Status"')
            sh(dev, 'draw text 4 4 "SD card ready"')
            sh(dev, 'draw text 4 6 "WiFi connected"')
            sh(dev, 'draw box 42 2 36 10 double "Tasks"')
            sh(dev, 'draw text 44 4 "1. shell"')
            sh(dev, 'draw text 44 6 "2. editor"')
            shot(dev, "tui-boxes")
            sh(dev, "draw close")

            # --- gfx canvas ---
            print("== gfx-demo ==")
            sh(dev, "gfx init 320 200")
            sh(dev, "gfx clear 0")
            sh(dev, "gfx rect 10 10 100 60 15")
            sh(dev, "gfx circle 200 100 40 14")
            sh(dev, "gfx line 0 190 319 10 12")
            sh(dev, "gfx text 10 170 15 P4MiniShell gfx")
            sh(dev, "gfx show")
            shot(dev, "gfx-demo")
            sh(dev, "gfx close")

            # --- plot graph (radians like PLOT.BAT; restore degrees after) ---
            print("== plot-graph ==")
            sh(dev, "calc /rad")
            sh(dev, "gfx init 320 200")
            sh(dev, "gfx clear 0")
            sh(dev, "plot window -6.28 6.28 -1.6 1.6")
            sh(dev, "plot axes /grid")
            sh(dev, 'plot func "sin(X)" 15', 3.0)
            sh(dev, 'plot func "cos(X)" 11', 3.0)
            sh(dev, "gfx show")
            shot(dev, "plot-graph")
            sh(dev, "gfx close")
            sh(dev, "calc /deg")

            # --- amber theme (session-only, restored after) ---
            print("== theme-amber ==")
            sh(dev, "theme set amber")
            sh(dev, "cls")
            shot(dev, "theme-amber")
            sh(dev, "theme set default")
            sh(dev, "cls")
        except Exception as exc:  # noqa: BLE001
            print("CAPTURE FAILED: %r" % exc)
            fails.append(str(exc))

    # report what landed
    want = ("shell-idle", "shell-help", "shell-dir", "apps-launch",
            "modal-dialog", "modal-list", "editor-editing", "tui-boxes",
            "gfx-demo", "plot-graph", "theme-amber")
    missing = [n for n in want
               if not os.path.isfile(os.path.join(OUT, n + ".png"))]
    if missing:
        print("MISSING: %s" % ", ".join(missing))
        return 1
    if fails:
        print("FAILURES: %s" % fails)
        return 1
    print("All %d shots captured in %s" % (11, OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
