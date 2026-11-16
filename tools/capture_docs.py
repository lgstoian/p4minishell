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

The capture is made deterministic where the UI allows it (fixed wall clock,
steady cursor, an established Wi-Fi session) so `tools/screenshot_gate.py`
can compare frames pixel for pixel. The header bar carries live telemetry
(heap, CPU, battery) and is masked by the gate, not by this script.

Usage:
  python tools/capture_docs.py [COMx]            # capture every state
  python tools/capture_docs.py COM3 --only shell-idle,shell-dir
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


def shot(dev, name, out_dir):
    bmp = dev.screenshot()
    path = os.path.join(out_dir, name + ".png")
    bmp.save_png(path)
    print("  saved %s (%dx%d)" % (path, bmp.width, bmp.height))


def prelude(dev):
    """Put the board in a reproducible state before any capture.

    - fixed wall clock: the header clock and any timestamp text stop moving;
    - steady cursor: the prompt caret no longer blinks between frames;
    - station disconnected: `wifi disconnect` clears the retry target and sets
      autoretry off, so the reconnect watchdog stops writing into the
      transcript (the header bar with its live link state is masked by the
      gate anyway).
    """
    sh(dev, "date 01-01-2026", 1.0)
    sh(dev, "time 12:00:00", 1.0)
    sh(dev, "cursor block blink 0", 1.0)
    sh(dev, "theme set default", 1.0)
    sh(dev, "wifi disconnect", 2.0)
    time.sleep(2.0)
    dev.read_for(1.0)
    sh(dev, "cls", 1.0)


def state_shell_idle(dev, out):
    sh(dev, "cls")
    shot(dev, "shell-idle", out)


def state_shell_help(dev, out):
    sh(dev, "cls")
    sh(dev, "help", 2.0)
    shot(dev, "shell-help", out)


def state_shell_dir(dev, out):
    sh(dev, "cls")
    sh(dev, "dir", 2.0)
    shot(dev, "shell-dir", out)


def state_apps_launch(dev, out):
    sh(dev, "cls")
    # The transcript scroll animation can still be moving a couple of seconds
    # after a long listing; capture once it has settled or the rows overlap.
    sh(dev, "launch /list", 5.0)
    shot(dev, "apps-launch", out)


def state_modal_dialog(dev, out):
    sh(dev, "cls")
    dev.send('dialog /t:10 "Delete file?" "Remove NOTES.TXT permanently?" Yes No')
    time.sleep(2.5)
    shot(dev, "modal-dialog", out)
    time.sleep(9.0)
    dev.read_for(2.0)


def state_modal_list(dev, out):
    sh(dev, "cls")
    dev.send('list /t:10 "Pick a colour" Red Green Blue Amber')
    time.sleep(2.5)
    shot(dev, "modal-list", out)
    time.sleep(9.0)
    dev.read_for(2.0)


def state_editor_editing(dev, out):
    sh(dev, "cls")
    dev.send("edit CONFIG.SYS")
    time.sleep(3.0)
    shot(dev, "editor-editing", out)
    dev.send("\\q")
    time.sleep(1.5)
    dev.read_for(2.0)


def state_tui_boxes(dev, out):
    sh(dev, "draw clear screen")
    sh(dev, 'draw box 2 2 36 10 single "Status"')
    sh(dev, 'draw text 4 4 "SD card ready"')
    sh(dev, 'draw text 4 6 "WiFi connected"')
    sh(dev, 'draw box 42 2 36 10 double "Tasks"')
    sh(dev, 'draw text 44 4 "1. shell"')
    sh(dev, 'draw text 44 6 "2. editor"')
    shot(dev, "tui-boxes", out)
    sh(dev, "draw close")


def state_gfx_demo(dev, out):
    sh(dev, "gfx init 320 200")
    sh(dev, "gfx clear 0")
    sh(dev, "gfx rect 10 10 100 60 15")
    sh(dev, "gfx circle 200 100 40 14")
    sh(dev, "gfx line 0 190 319 10 12")
    sh(dev, "gfx text 10 170 15 P4MiniShell gfx")
    sh(dev, "gfx show")
    shot(dev, "gfx-demo", out)
    sh(dev, "gfx close")


def state_plot_graph(dev, out):
    sh(dev, "calc /rad")
    sh(dev, "gfx init 320 200")
    sh(dev, "gfx clear 0")
    sh(dev, "plot window -6.28 6.28 -1.6 1.6")
    sh(dev, "plot axes /grid")
    sh(dev, 'plot func "sin(X)" 15', 3.0)
    sh(dev, 'plot func "cos(X)" 11', 3.0)
    sh(dev, "gfx show")
    shot(dev, "plot-graph", out)
    sh(dev, "gfx close")
    sh(dev, "calc /deg")


def state_theme_amber(dev, out):
    sh(dev, "theme set amber")
    # A theme change rebuilds the input line, which resets the caret to the
    # theme's blink period; pin it again before the capture.
    sh(dev, "cursor block blink 0", 0.5)
    sh(dev, "cls")
    shot(dev, "theme-amber", out)
    sh(dev, "theme set default")
    sh(dev, "cursor block blink 0", 0.5)
    sh(dev, "cls")


STATES = [
    ("shell-idle", state_shell_idle),
    ("shell-help", state_shell_help),
    ("shell-dir", state_shell_dir),
    ("apps-launch", state_apps_launch),
    ("modal-dialog", state_modal_dialog),
    ("modal-list", state_modal_list),
    ("editor-editing", state_editor_editing),
    ("tui-boxes", state_tui_boxes),
    ("gfx-demo", state_gfx_demo),
    ("plot-graph", state_plot_graph),
    ("theme-amber", state_theme_amber),
]


def capture_states(dev, out_dir=OUT, only=None, settle=4.0):
    """Run the state list (optionally a subset) and write PNGs.

    Returns the list of names actually captured. `settle` is the initial
    boot settle; the gate passes 0 when the board is already up.
    """
    os.makedirs(out_dir, exist_ok=True)
    want = set(only) if only else None
    done = []
    prelude(dev)
    # Warm-up frame: the first screenshot after a boot can leave the submitted
    # `screenshot` line in the transcript (it is captured while the command is
    # still being submitted). Discard one frame, then clear the screen so every
    # recorded state starts from the same clean transcript.
    try:
        dev.screenshot()
    except Exception:  # noqa: BLE001 - a warm-up miss is not fatal
        pass
    sh(dev, "cls", 1.0)
    for name, fn in STATES:
        if want is not None and name not in want:
            continue
        print("== %s ==" % name)
        fn(dev, out_dir)
        done.append(name)
    return done


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1].split(",")
    fails = []
    with Device(port, boot=True, settle_ms=4000) as dev:
        try:
            capture_states(dev, OUT, only=only)
        except Exception as exc:  # noqa: BLE001
            print("CAPTURE FAILED: %r" % exc)
            fails.append(str(exc))

    want = [n for n, _ in STATES if only is None or n in only]
    missing = [n for n in want
               if not os.path.isfile(os.path.join(OUT, n + ".png"))]
    if missing:
        print("MISSING: %s" % ", ".join(missing))
        return 1
    if fails:
        print("FAILURES: %s" % fails)
        return 1
    print("All %d shots captured in %s" % (len(want), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
