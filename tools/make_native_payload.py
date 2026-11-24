#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_native_payload.py - build, sign and check one signed .P4X payload.

The device has exactly ONE loader path (components/nativeexec) and one trust
key (`p4sign`). This host tool wraps a flat, fixed-ABI payload in the ABI.md
§4.1 envelope and signs the canonical bytes with a `pkg_sign.py` P-256 key.
It never installs a key on the device and never enables execution by itself:
launch remains opt-in through the app's `.bat` shim.

Usage:
  python tools/make_native_payload.py build --source demo.c --out DEMO.P4X \
      --key key.priv.pem [--entry 0] [--api console,env] [--check] [--cc cc]
  python tools/make_native_payload.py build --blob demo.bin --out DEMO.P4X \
      --key key.priv.pem [--entry 0] [--api console,env] [--check]
  python tools/make_native_payload.py check --in DEMO.P4X [--pub key.pub.bin]
  python tools/make_native_payload.py verify --in DEMO.P4X --pub key.pub.bin
  python tools/make_native_payload.py selftest        # red canary

Exit codes: 0 ok / 1 verification or build failure / 2 usage.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HEADER_BYTES = 148
MAGIC = b"P4NATPAY"
ARCH = b"esp32p4"
ABI = b"applib-1"
FORMAT_VERSION = 1
MAX_PAYLOAD = 2097152
SIG_BYTES = 64
PUB_BYTES = 64
SIG_OFF = 76
TAIL_OFF = 140

EX_MAGIC = b"P4EX"
EX_VERSION = 1
EX_HEADER_BYTES = 8
EX_ALIGN = 4
MAX_EXPORTS = 16

API_BITS = {
    "console": 1 << 0,
    "env": 1 << 1,
    "state": 1 << 2,
    "input": 1 << 3,
    "tui": 1 << 4,
    "modal": 1 << 5,
    "db": 1 << 6,
    "time": 1 << 7,
    "webapp": 1 << 8,
}
KNOWN_MASK = sum(API_BITS.values())
SUPPORTED_MASK = (API_BITS["console"] | API_BITS["env"] |
                  API_BITS["state"] | API_BITS["time"])


def _crypto():
    try:
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import (
            decode_dss_signature, encode_dss_signature, Prehashed)
        from cryptography.hazmat.primitives import hashes, serialization
        return ec, hashes, serialization, decode_dss_signature, encode_dss_signature, Prehashed
    except ImportError:
        print("FAIL: need the 'cryptography' package (pip install cryptography)")
        return None


def _err(text: str) -> int:
    print("FAIL: " + text)
    return 1


def canonical(header: bytes, payload: bytes) -> bytes:
    """Header without the self-referential SHA-256 and signature fields, then
    payload. Binding magic..crc + entry/reserved/api + payload leaves no
    circular dependency on the digest itself."""
    return header[:44] + header[TAIL_OFF:HEADER_BYTES] + payload


def api_mask(names: str) -> int:
    mask = 0
    for name in [n.strip().lower() for n in names.split(",") if n.strip()]:
        if name not in API_BITS:
            raise ValueError("unknown api group: " + name)
        mask |= API_BITS[name]
    return mask


def api_names(mask: int) -> str:
    return ",".join(n for n, b in API_BITS.items() if mask & b) or "(none)"


def build_header(payload: bytes, entry: int, api: int, digest: bytes,
                 signature: bytes | None) -> bytes:
    head = bytearray(HEADER_BYTES)
    head[0:8] = MAGIC
    struct.pack_into("<I", head, 8, FORMAT_VERSION)
    head[12:12 + len(ARCH)] = ARCH
    head[20:20 + len(ABI)] = ABI
    struct.pack_into("<I", head, 36, len(payload))
    struct.pack_into("<I", head, 40, zlib.crc32(payload) & 0xFFFFFFFF)
    head[44:76] = digest
    if signature is not None:
        head[76:140] = signature
    struct.pack_into("<H", head, 140, entry)
    struct.pack_into("<H", head, 142, 0)
    struct.pack_into("<I", head, 144, api)
    return bytes(head)


def sign_blob(payload: bytes, entry: int, api: int, key_path: str) -> bytes | None:
    mod = _crypto()
    if mod is None:
        return None
    ec, hashes, serialization, decode_dss, _encode_dss, Prehashed = mod
    with open(key_path, "rb") as fh:
        priv = serialization.load_pem_private_key(fh.read(), password=None)
    if not isinstance(priv, ec.EllipticCurvePrivateKey) or \
            not isinstance(priv.curve, ec.SECP256R1):
        print("FAIL: key is not an ECDSA P-256 private key")
        return None

    placeholder = build_header(payload, entry, api, bytes(32), None)
    digest = hashlib.sha256(canonical(placeholder, payload)).digest()
    der = priv.sign(digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    r, s = decode_dss(der)
    signature = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return build_header(payload, entry, api, digest, signature) + payload


def check_blob(blob: bytes, pub: bytes | None = None,
               require_signature: bool = False) -> tuple[int, dict]:
    """Pure envelope + fixed-ABI check. No file I/O, no execution."""
    info: dict = {}
    if len(blob) < HEADER_BYTES:
        return _err("header truncated"), info
    head = blob[:HEADER_BYTES]
    if head[:8] != MAGIC:
        return _err("bad magic"), info
    if struct.unpack_from("<I", head, 8)[0] != FORMAT_VERSION:
        return _err("bad format version"), info
    if head[12:20].rstrip(b"\0") != ARCH:
        return _err("arch mismatch"), info
    if head[20:36].rstrip(b"\0") != ABI:
        return _err("abi mismatch"), info
    size = struct.unpack_from("<I", head, 36)[0]
    if size == 0 or size > MAX_PAYLOAD:
        return _err("payload size out of range"), info
    if size != len(blob) - HEADER_BYTES:
        return _err("payload size does not match blob length"), info
    if struct.unpack_from("<H", head, 142)[0] != 0:
        return _err("reserved field set"), info
    api = struct.unpack_from("<I", head, 144)[0]
    entry = struct.unpack_from("<H", head, 140)[0]
    if api & ~KNOWN_MASK:
        return _err("unknown required api bits"), info
    if api & ~SUPPORTED_MASK:
        return _err("unsupported required api bits (loader supports console,env,state,time)"), info
    payload = blob[HEADER_BYTES:]
    info.update(size=size, api=api, entry=entry)

    if struct.unpack_from("<I", head, 40)[0] != (zlib.crc32(payload) & 0xFFFFFFFF):
        return _err("payload crc mismatch"), info

    digest = hashlib.sha256(canonical(head, payload)).digest()
    if head[44:76] != digest:
        return _err("payload sha256 mismatch"), info

    # Fixed ABI: no ELF, no relocation table, one export array at offset 0.
    if len(payload) < EX_HEADER_BYTES or payload[:4] != EX_MAGIC:
        return _err("payload abi magic mismatch (no ELF/reloc path)"), info
    if struct.unpack_from("<H", payload, 4)[0] != EX_VERSION:
        return _err("payload abi version mismatch"), info
    count = struct.unpack_from("<H", payload, 6)[0]
    if count == 0 or count > MAX_EXPORTS:
        return _err("bad export count"), info
    table_end = EX_HEADER_BYTES + 4 * count
    if table_end > len(payload):
        return _err("export table truncated"), info
    if entry >= count:
        return _err("entry ordinal out of range"), info
    offset = struct.unpack_from("<I", payload, EX_HEADER_BYTES + 4 * entry)[0]
    if offset < table_end or offset >= len(payload) or offset % EX_ALIGN:
        return _err("entry offset out of range or unaligned"), info
    info.update(exports=count, entry_offset=offset)

    if pub is None:
        if require_signature:
            return _err("no trusted public key supplied"), info
        info["signature"] = "not checked (no --pub)"
        return 0, info
    if len(pub) != PUB_BYTES:
        return _err("pubkey file must hold exactly 64 raw bytes (X||Y)"), info
    mod = _crypto()
    if mod is None:
        return 1, info
    ec, hashes, _serialization, _decode, encode_dss, Prehashed = mod
    point = ec.EllipticCurvePublicNumbers(
        int.from_bytes(pub[:32], "big"), int.from_bytes(pub[32:], "big"),
        ec.SECP256R1()).public_key()
    sig = head[76:140]
    try:
        point.verify(encode_dss(int.from_bytes(sig[:32], "big"),
                                int.from_bytes(sig[32:], "big")),
                     digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    except Exception as exc:
        return _err("bad signature (%s)" % exc), info
    info["signature"] = "ok"
    return 0, info


def payload_from_source(source: str, cc: str) -> bytes | None:
    """Compile one tiny C file and concatenate its fixed export section + text.

    This is intentionally minimal: the source supplies the P4EX export table,
    and entry offsets are known constants because exports are always first.
    A missing cross toolchain is an explicit error, never a silent fake.
    """
    compiler = cc or os.environ.get("CC") or "cc"
    if shutil.which(compiler) is None:
        _err("C compiler '%s' not found; pass --cc or set CC" % compiler)
        return None
    with tempfile.TemporaryDirectory(prefix="p4native-") as td:
        obj = os.path.join(td, "payload.o")
        exp = os.path.join(td, "exports.bin")
        text = os.path.join(td, "text.bin")
        proc = subprocess.run([compiler, "-c", "-ffreestanding", "-O2",
                               "-fno-pie", source, "-o", obj],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stderr.strip())
            _err("C compilation failed")
            return None
        for section, out in ((".p4ex.exports", exp), (".text", text)):
            proc = subprocess.run(["objcopy", "--dump-section",
                                   "%s=%s" % (section, out), obj, obj],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                _err("cannot dump %s (need objcopy)" % section)
                return None
        with open(exp, "rb") as fh:
            exports = fh.read()
        with open(text, "rb") as fh:
            code = fh.read()
    if len(exports) < EX_HEADER_BYTES or exports[:4] != EX_MAGIC:
        _err("source has no .p4ex.exports section with P4EX magic")
        return None
    return exports + code


def cmd_build(args) -> int:
    if bool(args.source) == bool(args.blob):
        return _err("pass exactly one of --source or --blob")
    try:
        api = api_mask(args.api)
    except ValueError as exc:
        return _err(str(exc))
    if args.entry >= MAX_EXPORTS:
        return _err("entry ordinal must be < %d" % MAX_EXPORTS)

    if args.source:
        payload = payload_from_source(args.source, args.cc)
        if payload is None:
            return 1
    else:
        with open(args.blob, "rb") as fh:
            payload = fh.read()
    if not payload or len(payload) > MAX_PAYLOAD:
        return _err("payload must be 1..%d bytes" % MAX_PAYLOAD)

    blob = sign_blob(payload, args.entry, api, args.key)
    if blob is None:
        return 1
    with open(args.out, "wb") as fh:
        fh.write(blob)
    print("wrote %s (%d bytes, entry=%d, api=%s)" %
          (args.out, len(blob), args.entry, api_names(api)))

    if args.check:
        pub = None
        if args.pub:
            with open(args.pub, "rb") as fh:
                pub = fh.read()
        rc, info = check_blob(blob, pub, require_signature=bool(pub))
        if rc == 0:
            print("check OK: %s" % info)
        return rc
    return 0


def cmd_check(args) -> int:
    with open(args.input, "rb") as fh:
        blob = fh.read()
    pub = None
    if args.pub:
        with open(args.pub, "rb") as fh:
            pub = fh.read()
    rc, info = check_blob(blob, pub, require_signature=bool(pub))
    if rc == 0:
        print("check OK: %s" % info)
    return rc


def cmd_verify(args) -> int:
    with open(args.input, "rb") as fh:
        blob = fh.read()
    with open(args.pub, "rb") as fh:
        pub = fh.read()
    rc, info = check_blob(blob, pub, require_signature=True)
    if rc == 0:
        print("verify OK: signed by the supplied key; %s" % info)
    return rc


def cmd_selftest() -> int:
    """Red canary: every tampering must be refused, the clean blob accepted."""
    mod = _crypto()
    if mod is None:
        return 1
    ec, _hashes, serialization, _decode, _encode, _prehashed = mod
    with tempfile.TemporaryDirectory(prefix="p4native-selftest-") as td:
        priv = ec.generate_private_key(ec.SECP256R1())
        key = os.path.join(td, "test.priv.pem")
        with open(key, "wb") as fh:
            fh.write(priv.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption()))
        nums = priv.public_key().public_numbers()
        pub = (nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big"))

        payload = bytearray(EX_HEADER_BYTES + 4 * 2 + 8)
        payload[:4] = EX_MAGIC
        struct.pack_into("<H", payload, 4, EX_VERSION)
        struct.pack_into("<H", payload, 6, 2)
        struct.pack_into("<I", payload, 8, 16)
        struct.pack_into("<I", payload, 12, 20)
        clean = sign_blob(bytes(payload), 0, API_BITS["console"] | API_BITS["env"],
                          key)
        if clean is None:
            return 1
        rc, _ = check_blob(clean, pub, require_signature=True)
        if rc != 0:
            return _err("canary: clean signed payload was refused")

        bad_cases = {
            "magic": lambda b: b.__setitem__(slice(0, 8), b"BADMAGIC"),
            "version": lambda b: struct.pack_into("<I", b, 8, 2),
            "size": lambda b: struct.pack_into("<I", b, 36, 999),
            "crc": lambda b: b.__setitem__(40, b[40] ^ 1),
            "sha256": lambda b: b.__setitem__(44, b[44] ^ 1),
            "signature": lambda b: b.__setitem__(76, b[76] ^ 1),
            "reserved": lambda b: struct.pack_into("<H", b, 142, 1),
            "api bit": lambda b: struct.pack_into("<I", b, 144, 1 << 31),
            "unsupported api": lambda b: struct.pack_into("<I", b, 144,
                                                          API_BITS["webapp"]),
            "entry ordinal": lambda b: struct.pack_into("<H", b, 140, 9),
            "elf payload": lambda b: b.__setitem__(
                slice(HEADER_BYTES, HEADER_BYTES + 4), b"\x7fELF"),
            "entry offset": lambda b: struct.pack_into(
                "<I", b, HEADER_BYTES + 12, len(payload)),
        }
        failed = 0
        for name, mutate in bad_cases.items():
            tampered = bytearray(clean)
            mutate(tampered)
            rc, _ = check_blob(bytes(tampered), pub, require_signature=True)
            if rc == 0:
                print("FAIL: canary accepted tampering: %s" % name)
                failed += 1
    if failed:
        return _err("red canary failed for %d mutation(s)" % failed)
    print("PASS: red canary rejected all %d mutations; clean blob verified" %
          len(bad_cases))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="verb", required=True)

    build = sub.add_parser("build", help="wrap and sign a flat payload")
    src = build.add_mutually_exclusive_group(required=True)
    src.add_argument("--source", help="small C file with a .p4ex.exports section")
    src.add_argument("--blob", help="prebuilt flat payload bytes")
    build.add_argument("--out", required=True)
    build.add_argument("--key", required=True, help="pkg_sign.py P-256 private key")
    build.add_argument("--pub", help="public key for --check (optional)")
    build.add_argument("--entry", type=int, default=0)
    build.add_argument("--api", default="console",
                       help="comma-separated api groups (console,env,state,time)")
    build.add_argument("--cc", default="", help="C compiler for --source")
    build.add_argument("--check", action="store_true", help="check the output")
    build.set_defaults(func=cmd_build)

    check = sub.add_parser("check", help="check envelope + fixed ABI")
    check.add_argument("--in", dest="input", required=True)
    check.add_argument("--pub", help="also verify the signature")
    check.set_defaults(func=cmd_check)

    verify = sub.add_parser("verify", help="verify signature with a trusted key")
    verify.add_argument("--in", dest="input", required=True)
    verify.add_argument("--pub", required=True)
    verify.set_defaults(func=cmd_verify)

    selftest = sub.add_parser("selftest", help="red canary for the checker")
    selftest.set_defaults(func=lambda _args: cmd_selftest())

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ValueError) as exc:
        return _err(str(exc))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
