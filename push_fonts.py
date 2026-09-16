#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_fonts.py - upload the bundled TTF/OTF fonts to sd:/FONTS/.

Pushes assets/fonts/*.ttf|*.otf the same way apps/push_apps.py pushes apps:
ACK-paced `receive <path> <size> /crc` binary transfer (byte-exact, CRC-32
verified, partials removed on mismatch). Licenses + SHA256SUMS stay in the
repo (not pushed).

Usage:
    python push_fonts.py [COMx]
"""

import glob
import os
import serial
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "apps", "companion"))
from push_sd import (  # noqa: E402
    push_file,
    wait_shell,
)

REPO_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    fonts = sorted(glob.glob(os.path.join(REPO_DIR, "assets", "fonts", "*.ttf")) +
                   glob.glob(os.path.join(REPO_DIR, "assets", "fonts", "*.otf")))
    if not fonts:
        print("FAIL: no fonts in assets/fonts")
        return 1
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
    # boot quiesce before the ACK-paced transfers start (17 MB takes a while).
    time.sleep(20.0)
    ser.reset_input_buffer()

    ser.reset_input_buffer()
    ser.write(b"md FONTS\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)

    ok = True
    for path in fonts:
        name = os.path.basename(path)
        target = "FONTS/" + name
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
