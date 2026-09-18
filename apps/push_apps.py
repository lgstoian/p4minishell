#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_apps.py - upload the P4 reference apps to the SD card.

Pushes apps/adventure, apps/notes and apps/mood the same way
apps/companion/push_sd.py pushes the Companion: entry .BATs go to the SD
root (on PATH, so `launch` finds them), .APPINFO metadata goes to sd:/APPS.

Usage:
    python push_apps.py [COMx]
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import (  # noqa: E402
    push_file,
    wait_shell,
    open_port,
    READY,
)

APPS_DIR = os.path.dirname(os.path.abspath(__file__))
FILES = [
    ("adventure", "ADVENT.BAT", False),
    ("adventure", "ADVENT.APPINFO", True),
    ("adventure", "ALIASES.BAT", False),
    ("notes", "NOTES.BAT", False),
    ("notes", "NOTES.APPINFO", True),
    ("notes", "ALIASES.BAT", False),
    ("mood", "MOOD.BAT", False),
    ("mood", "MOOD.APPINFO", True),
    ("mood", "ALIASES.BAT", False),
    ("gfxdemo", "BOUNCE.BAT", False),
    ("gfxdemo", "BOUNCE.APPINFO", True),
    ("gfxdemo", "GFXTOOL.BAT", False),
    ("gfxdemo", "GFXTOOL.APPINFO", True),
    ("gfxdemo", "PLOT.BAT", False),
    ("gfxdemo", "PLOT.APPINFO", True),
    ("snake", "SNAKE.BAT", False),
    ("snake", "SNAKE.APPINFO", True),
    ("tcmd", "TCMD.BAT", False),
    ("tcmd", "TCMD.APPINFO", True),
    ("elite", "ELITE.BAT", False),
    ("elite", "ELITE.APPINFO", True),
    ("pics", "PICS.BAT", False),
    ("pics", "PICS.APPINFO", True),
    ("uitest", "UITEST.BAT", False),
    ("uitest", "UITEST.APPINFO", True),
    ("palmtop", "PALMTOP.BAT", False),
    ("palmtop", "PALMTOP.APPINFO", True),
    ("diag", "DIAG.BAT", False),
    ("diag", "DIAG.APPINFO", True),
    ("controlflow", "CONTROL.BAT", False),
    ("controlflow", "CONTROL.APPINFO", True),
    ("writer", "WRITER.BAT", False),
    ("writer", "WRITER.APPINFO", True),
]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()

    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port)
        ser.close()
        return 1

    # The open above usually reboots the board (DTR transition); let the
    # boot quiesce before the ACK-paced transfers start.
    time.sleep(20.0)
    ser.reset_input_buffer()

    ser.reset_input_buffer()
    ser.write(b"md APPS\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)

    ok = True
    for subdir, name, appinfo in FILES:
        path = os.path.join(APPS_DIR, subdir, name)
        target = ("APPS/" + name) if appinfo else name
        with open(path, "rb") as f:
            data = f.read()
        pushed = False
        for attempt in range(3):
            if push_file(ser, target, data):
                print("PUSH ok   %s (%d bytes)" % (target, len(data)))
                pushed = True
                break
            print("  retry %s (%d/3)" % (target, attempt + 1))
            time.sleep(2.0)
        if not pushed:
            print("PUSH FAIL %s" % target)
            ok = False

    ser.close()
    print("PUSH", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
