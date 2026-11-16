#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_pkg.py - build a single-file `.PKG` bundle archive for `pkg`.

A `.PKG` is a ZIP container whose entries mirror the `PKGS/<APP>/` directory
layout:

    <APP>/<APP>.ASSETS    `path=HEXCRC` manifest (+ optional `SIGN=`)
    <APP>/<APP>.APPINFO   metadata
    <APP>/<path>          each payload at its install-relative path

On the board `pkg install <APP>` unpacks it through the ONE ZIP reader
(`components/zip`) and then runs the ordinary directory-bundle flow;
`pkg unpack <APP>` does only the unpack. See docs/native_packaging.md.

Usage:
  python tools/make_pkg.py <app> [--src apps/<app>] [--out PKGS/<app>.PKG]
  python tools/make_pkg.py <app> --bundle PKGS/<app> [--sign NAME.priv.pem]

`--src` points at a directory holding `<APP>.APPINFO` and the payloads at their
install-relative paths; `--bundle` reads an already-assembled `PKGS/<APP>/`.
Without either, `PKGS/<APP>/` is used. `--sign` adds an ECDSA P-256 `SIGN=`
line over the canonical manifest bytes (see tools/pkg_sign.py).
"""

import argparse
import os
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from pkg_sign import canonical  # noqa: E402

MANIFEST_EXT = ".ASSETS"
APPINFO_EXT = ".APPINFO"


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


def collect(src_dir, app):
    """Return [(zip_name, bytes)] for every payload at its install-relative path."""
    entries = []
    for base, _dirs, files in os.walk(src_dir):
        for name in sorted(files):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, src_dir).replace(os.sep, "/")
            if rel == app + MANIFEST_EXT or rel == app + APPINFO_EXT:
                continue
            with open(full, "rb") as f:
                entries.append((rel, f.read()))
    return entries


def build(app, src_dir, out_path, keypath=None):
    manifest = ["; %s bundle manifest (pkg install %s)\n" % (app, app)]
    entries = []
    for rel, data in collect(src_dir, app):
        manifest.append("%s=%s\n" % (rel, crc_hex(data)))
        entries.append(("%s/%s" % (app, rel), data))

    appinfo_rel = app + APPINFO_EXT
    appinfo_path = os.path.join(src_dir, appinfo_rel)
    if not os.path.exists(appinfo_path):
        print("FAIL: missing %s in %s" % (appinfo_rel, src_dir))
        return 1
    with open(appinfo_path, "rb") as f:
        appinfo = f.read()
    entries.append(("%s/%s" % (app, appinfo_rel), appinfo))

    body = "".join(manifest).encode()
    if keypath is not None:
        body = sign_manifest(body, keypath)
    entries.append(("%s/%s" % (app, app + MANIFEST_EXT), body))

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in entries:
            zf.writestr(name, data)

    print("wrote %s (%d payload(s), %d bytes)"
          % (out_path, len(entries) - 2, os.path.getsize(out_path)))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Build a .PKG bundle archive.")
    ap.add_argument("app", nargs="?", help="app name (matches <APP>.APPINFO)")
    ap.add_argument("--src", help="source dir with <APP>.APPINFO + payloads")
    ap.add_argument("--bundle", help="read an assembled PKGS/<APP>/ bundle")
    ap.add_argument("--out", help="output path (default PKGS/<APP>.PKG)")
    ap.add_argument("--sign", help="private key path for an ECDSA P-256 SIGN line")
    args = ap.parse_args()

    if not args.app:
        ap.print_help()
        return 2

    if args.bundle:
        src_dir = args.bundle
    elif args.src:
        src_dir = args.src
    else:
        src_dir = os.path.join("PKGS", args.app)
    if not os.path.isdir(src_dir):
        print("FAIL: bundle source not found: %s" % src_dir)
        return 1

    out_path = args.out or os.path.join("PKGS", args.app + ".PKG")
    return build(args.app, src_dir, out_path, args.sign)


if __name__ == "__main__":
    sys.exit(main())
