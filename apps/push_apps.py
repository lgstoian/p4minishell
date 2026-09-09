#!/usr/bin/env python3
"""push_apps.py - upload the P4 reference apps to the SD card.

Pushes apps/adventure, apps/notes and apps/mood the same way
apps/companion/push_sd.py pushes the Companion: entry .BATs go to the SD
root (on PATH, so `launch` finds them), .APPINFO metadata goes to sd:/APPS.

Usage:
    python push_apps.py [COMx]
"""

import os
import serial
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import (  # noqa: E402
    push_file,
    wait_shell,
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
]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = serial.Serial(port, 115200, timeout=1)
    ser.setDTR(False)
    ser.setRTS(False)  # open must not reboot the P4
    time.sleep(0.5)
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
