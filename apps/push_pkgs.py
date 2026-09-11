#!/usr/bin/env python3
"""push_pkgs.py - build + push SD app packages (bundles) to the board.

For each reference app it creates `PKGS/<APP>/` containing the app payload
files at the bundle root, the app's `.APPINFO`, and a `<APP>.ASSETS` manifest
(`path=HEXCRC`). On the board `pkg install <APP>` copies those into place and
`pkg verify <APP>` re-checks them. Manual install path; push once, install on
device.

Usage: python push_pkgs.py [COMx]
"""
import os
import sys
import time
import zlib

import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import push_file, wait_shell  # noqa: E402

APPS_DIR = os.path.dirname(os.path.abspath(__file__))

# (app, bundle source dir, payload files, appinfo file)
PACKAGES = [
    ("PKGTEST", "pkgtest", ["PKGTEST.BAT"], "PKGTEST.APPINFO"),
    ("BOUNCE", "gfxdemo", ["BOUNCE.BAT"], "BOUNCE.APPINFO"),
    ("GFXTOOL", "gfxdemo", ["GFXTOOL.BAT"], "GFXTOOL.APPINFO"),
    ("PLOT", "gfxdemo", ["PLOT.BAT"], "PLOT.APPINFO"),
    ("SNAKE", "snake", ["SNAKE.BAT"], "SNAKE.APPINFO"),
    ("TCMD", "tcmd", ["TCMD.BAT"], "TCMD.APPINFO"),
    ("ELITE", "elite", ["ELITE.BAT"], "ELITE.APPINFO"),
]


def read(path):
    with open(path, "rb") as f:
        return f.read()


def crc_hex(data):
    return "%08X" % (zlib.crc32(data) & 0xFFFFFFFF)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = serial.Serial(port, 115200, timeout=1)
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.5)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port)
        ser.close()
        return 1
    time.sleep(20.0)
    ser.reset_input_buffer()

    def shell(cmd, wait=0.6):
        ser.write((cmd + "\n").encode())
        time.sleep(wait)
        ser.read(ser.in_waiting or 1)

    shell("md PKGS")
    shell("md PKGS/PKGTEST")

    ok = True
    for app, subdir, payloads, appinfo in PACKAGES:
        shell("md PKGS/%s" % app)
        manifest = []
        for name in payloads:
            data = read(os.path.join(APPS_DIR, subdir, name))
            manifest.append("%s=%s" % (name, crc_hex(data)))
            if not push_file(ser, "PKGS/%s/%s" % (app, name), data):
                print("FAIL payload %s/%s" % (app, name))
                ok = False
        ai = read(os.path.join(APPS_DIR, subdir, appinfo))
        if not push_file(ser, "PKGS/%s/%s" % (app, appinfo), ai):
            print("FAIL appinfo %s" % app)
            ok = False
        body = ("; %s bundle manifest (pkg install %s)\n" % (app, app)).encode()
        for line in manifest:
            body += (line + "\n").encode()
        if not push_file(ser, "PKGS/%s/%s.ASSETS" % (app, app), body):
            print("FAIL manifest %s" % app)
            ok = False
        print("BUNDLE ok   %s (%d payloads)" % (app, len(manifest)))
    ser.close()
    print("RESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
