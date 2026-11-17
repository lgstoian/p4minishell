#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_apps.py - upload every P4 reference app to the SD card.

The payload list lives in ``apps/app_manifest.py`` (single source shared with
``apps/companion/push_sd.py`` and ``tools/suites/s14_apps.py``): entry .BATs go
to the SD root (on PATH, so ``launch`` finds them), .APPINFO metadata goes to
sd:/APPS.

Usage:
    python push_apps.py [COMx] [--drive sd:|usb:|usbN:]

The same payload runs from SD or a USB stick (v1.4.3): `--drive usb:` writes
the identical bundle to usb:/APPS + usb:/WEB, and the shell's `launch`, path
resolution and `%APPDRIVE%` make the apps behave identically there.
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from push_sd import (  # noqa: E402
    push_file,
    wait_shell,
    open_port,
    READY,
)
from app_manifest import deploy_list  # noqa: E402

APPS_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    args = sys.argv[1:]
    drive = "sd:"
    if "--drive" in args:
        idx = args.index("--drive")
        if idx + 1 < len(args):
            drive = args[idx + 1].rstrip("/")
            del args[idx:idx + 2]
    port = args[0] if args else "COM11"
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
    for directory in (b"APPS", b"WEB", b"EMOJI"):
        ser.write(("md %s/" % drive).encode() + directory + b"\n")
        time.sleep(0.5)
        ser.read(ser.in_waiting or 1)

    ok = True
    for path, target in deploy_list():
        target = "%s/%s" % (drive, target) if drive != "sd:" else target
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
