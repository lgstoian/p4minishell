#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""pkg_sign.py - ECDSA P-256 manifest keys and signatures for `pkg`.

The device verifies `SIGN=<128 hex>` (raw r||s) over the canonical manifest
bytes with the trusted raw X||Y key in its `p4sign` NVS store (see ABI.md).
The private key never leaves the host.

Usage:
  python tools/pkg_sign.py keygen --out NAME      # NAME.priv.pem + NAME.pub.bin + fingerprint
  python tools/pkg_sign.py sign <bundle.ASSETS> --key NAME.priv.pem
  python tools/pkg_sign.py verify <bundle.ASSETS> --pub NAME.pub.bin
"""
import hashlib
import os
import sys


def _require_crypto():
    try:
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric.utils import (
            decode_dss_signature, encode_dss_signature)
        return ec, hashes, serialization, decode_dss_signature, encode_dss_signature
    except ImportError:
        print("FAIL: need the 'cryptography' package (pip install cryptography)")
        return None


def canonical(data):
    """Canonical signed bytes: every line except blanks, #/; comments and
    SIGN lines, one trailing CR stripped, each + LF. Mirrors
    pkg_sign_canonical() in components/command/pkg_commands.c exactly."""
    out = bytearray()
    for raw in data.split(b"\n"):
        line = raw[:-1] if raw.endswith(b"\r") else raw
        stripped = line.lstrip(b" \t")
        if not stripped:
            continue
        if stripped[:1] in (b"#", b";"):
            continue
        if stripped[:5].upper() == b"SIGN=":
            continue
        out += line + b"\n"
    return bytes(out)


def cmd_keygen(args):
    mod = _require_crypto()
    if mod is None:
        return 1
    ec, hashes, serialization, decode_dss_signature, encode_dss_signature = mod
    if len(args) != 1:
        print("Usage: pkg_sign.py keygen --out NAME")
        return 2
    name = args[0]
    priv = ec.generate_private_key(ec.SECP256R1())
    nums = priv.public_key().public_numbers()
    pub_raw = nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")
    with open(name + ".priv.pem", "wb") as f:
        f.write(priv.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
    with open(name + ".pub.bin", "wb") as f:
        f.write(pub_raw)
    print("wrote %s.priv.pem (KEEP SECRET) + %s.pub.bin" % (name, name))
    print("fingerprint: %s" % hashlib.sha256(pub_raw).hexdigest())
    print("install on device: push %s.pub.bin, then: pkg key install <file>" % name)
    return 0


def cmd_sign(args):
    mod = _require_crypto()
    if mod is None:
        return 1
    ec, hashes, serialization, decode_dss_signature, encode_dss_signature = mod
    if len(args) != 2:
        print("Usage: pkg_sign.py sign <bundle.ASSETS> --key NAME.priv.pem")
        return 2
    manifest, keypath = args
    with open(keypath, "rb") as f:
        priv = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(priv, ec.EllipticCurvePrivateKey) or \
            not isinstance(priv.curve, ec.SECP256R1):
        print("FAIL: key is not an ECDSA P-256 private key")
        return 1
    with open(manifest, "rb") as f:
        data = f.read()
    canon = canonical(data)
    sig_der = priv.sign(canon, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(sig_der)
    sigline = "SIGN=%064x%064x" % (r, s)
    kept = [ln for ln in data.split(b"\n")
            if not ln.lstrip(b" \t")[:5].upper() == b"SIGN="]
    # Drop the empty tail the split leaves on a trailing newline, then
    # re-terminate exactly like the input.
    if kept and kept[-1] == b"":
        kept = kept[:-1]
    body = sigline.encode() + b"\n" + b"\n".join(kept)
    if data.endswith(b"\n"):
        body += b"\n"
    with open(manifest, "wb") as f:
        f.write(body)
    print("signed %s (%d canonical bytes)" % (manifest, len(canon)))
    return 0


def cmd_verify(args):
    mod = _require_crypto()
    if mod is None:
        return 1
    ec, hashes, serialization, decode_dss_signature, encode_dss_signature = mod
    if len(args) != 2:
        print("Usage: pkg_sign.py verify <bundle.ASSETS> --pub NAME.pub.bin")
        return 2
    manifest, pubpath = args
    with open(pubpath, "rb") as f:
        pub_raw = f.read()
    if len(pub_raw) != 64:
        print("FAIL: pubkey file must hold exactly 64 raw bytes (X||Y)")
        return 1
    x = int.from_bytes(pub_raw[:32], "big")
    y = int.from_bytes(pub_raw[32:], "big")
    pub = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
    with open(manifest, "rb") as f:
        data = f.read()
    sigline = None
    for raw in data.split(b"\n"):
        line = raw[:-1] if raw.endswith(b"\r") else raw
        if line.lstrip(b" \t")[:5].upper() == b"SIGN=":
            sigline = line
            break
    if sigline is None:
        print("unsigned (no SIGN line)")
        return 0
    try:
        sig_raw = bytes.fromhex(sigline.split(b"=", 1)[1].decode().strip())
    except ValueError:
        print("FAIL: malformed SIGN line")
        return 1
    if len(sig_raw) != 64:
        print("FAIL: SIGN must hold 128 hex digits")
        return 1
    r = int.from_bytes(sig_raw[:32], "big")
    s = int.from_bytes(sig_raw[32:], "big")
    try:
        pub.verify(encode_dss_signature(r, s), canonical(data),
                   ec.ECDSA(hashes.SHA256()))
    except Exception as exc:
        print("FAIL: bad signature (%s)" % exc)
        return 1
    print("signed OK")
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("keygen", "sign", "verify"):
        print(__doc__)
        return 2
    verb = sys.argv[1]
    rest = sys.argv[2:]
    if verb == "keygen":
        if len(rest) == 2 and rest[0] == "--out":
            return cmd_keygen([rest[1]])
        print("Usage: pkg_sign.py keygen --out NAME")
        return 2
    if verb == "sign":
        if len(rest) == 3 and rest[1] == "--key":
            return cmd_sign([rest[0], rest[2]])
        print("Usage: pkg_sign.py sign <bundle.ASSETS> --key NAME.priv.pem")
        return 2
    if verb == "verify":
        if len(rest) == 3 and rest[1] == "--pub":
            return cmd_verify([rest[0], rest[2]])
        print("Usage: pkg_sign.py verify <bundle.ASSETS> --pub NAME.pub.bin")
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
