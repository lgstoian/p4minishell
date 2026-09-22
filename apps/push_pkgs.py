#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_pkgs.py - build + push SD app packages (bundles) to the board.

For each reference app it creates `PKGS/<APP>/` containing the app payload
files at the bundle root, the app's `.APPINFO`, and a `<APP>.ASSETS` manifest
(`path=HEXCRC`). On the board `pkg install <APP>` copies those into place and
`pkg verify <APP>` re-checks them. Manual install path; push once, install on
device.

With `--sign NAME.priv.pem` every manifest gains an ECDSA P-256 `SIGN=` line
(see ABI.md), so `pkg install` verifies it against the device's trusted key.

Usage: python push_pkgs.py [COMx] [--sign NAME.priv.pem]
"""
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import push_file, wait_shell, open_port  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from pkg_sign import canonical  # noqa: E402

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


def sign_manifest(body, keypath):
    """Prepend a `SIGN=<128 hex>` line (ECDSA P-256) over the canonical body."""
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric.utils import (
        decode_dss_signature)

    with open(keypath, "rb") as f:
        priv = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(priv, ec.EllipticCurvePrivateKey) or \
            not isinstance(priv.curve, ec.SECP256R1):
        raise ValueError("key is not an ECDSA P-256 private key")
    r, s = decode_dss_signature(priv.sign(canonical(body), ec.ECDSA(hashes.SHA256())))
    return ("SIGN=%064x%064x\n" % (r, s)).encode() + body


def main():
    args = sys.argv[1:]
    keypath = None
    if "--sign" in args:
        i = args.index("--sign")
        if i + 1 >= len(args):
            print("FAIL: --sign needs a private-key path")
            return 2
        keypath = args[i + 1]
        del args[i:i + 2]
    port = args[0] if args else "COM11"
    ser = open_port(port, 115200, 1)
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
        if keypath is not None:
            body = sign_manifest(body, keypath)
        if not push_file(ser, "PKGS/%s/%s.ASSETS" % (app, app), body):
            print("FAIL manifest %s" % app)
            ok = False
        print("BUNDLE ok   %s (%d payloads%s)" %
              (app, len(manifest), ", signed" if keypath else ""))
    ser.close()
    print("RESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
