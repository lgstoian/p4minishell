# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""radicale_fixture.py - local CardDAV test addressbook for `pim net` E2E.

Runs a Radicale server on the dev PC with a fixture addressbook (ASCII,
ampersand, and unicode display names - the paths `pim_carddav` must survive),
then self-checks it with a raw PROPFIND (no CardDAV library on the host:
urllib + http.client only, mirroring what the firmware does).

Usage:
    python tools/radicale_fixture.py serve [--port 5232] [--dir DIR]
        # writes DIR/collection-root/alice/contacts/{a,b,c}.vcf via HTTP PUT
        # (Radicale auto-creates collections on PUT), then serves blocking.
    python tools/radicale_fixture.py check [--port 5232]
        # PROPFIND Depth:1 on /alice/contacts/, asserts 207 + hrefs.

Board side (board and PC must share a LAN - phone hotspot works):
    pim net sync http://<PC-IP>:5232/alice/contacts/ alice addressbook /p:<pass>

Auth: none (local bench only, never exposed). Plain HTTP exercises the
CardDAV protocol; wrap with stunnel/Caddy for the TLS variant.
"""
from __future__ import annotations

import argparse
import http.client
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse

CONTACTS = {
    "adauid-1.vcf": (
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:adauid-1\r\nFN:Ada Tester\r\n"
        "TEL:+40123456789\r\nEND:VCARD\r\n"
    ),
    "bobuid-2.vcf": (
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:bobuid-2\r\nFN:Bob & Sons\r\n"
        "ORG:Bobs & Sons <Ltd>\r\nEND:VCARD\r\n"
    ),
    "jorguid-3.vcf": (
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:jorguid-3\r\nFN:J\u00f6rg \u2661\r\n"
        "NOTE:unicode \u2661 test\r\nEND:VCARD\r\n"
    ),
}

PROPFIND_BODY = (
    '<?xml version="1.0"?>'
    '<d:propfind xmlns:d="DAV:">'
    '<d:prop><d:resourcetype/><d:getetag/></d:prop>'
    "</d:propfind>"
).encode()

PROBE_VCARD = (
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:probe-uid-delete-me\r\nFN:Probe\r\n"
    "END:VCARD\r\n"
)

BENCH_USER = "alice"
BENCH_PASS = "benchpass123"


def _auth_header():
    import base64

    raw = ("%s:%s" % (BENCH_USER, BENCH_PASS)).encode()
    return "Basic " + base64.b64encode(raw).decode()


def put_card(host, port, base, name, body):
    conn = http.client.HTTPConnection(host, port, timeout=15)
    conn.request("PUT", base + name, body=body.encode(),
                 headers={"Content-Type": "text/vcard",
                          "Authorization": _auth_header()})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    if resp.status not in (200, 201, 204):
        raise RuntimeError("PUT %s -> %d %s" % (name, resp.status, data[:80]))
    return resp.status


MKCOL_BODY = (
    '<?xml version="1.0"?>'
    '<d:mkcol xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:carddav">'
    "<d:set><d:prop>"
    "<d:resourcetype><d:collection/><c:addressbook/></d:resourcetype>"
    "</d:prop></d:set>"
    "</d:mkcol>"
).encode()


def mkcol(host, port, base):
    conn = http.client.HTTPConnection(host, port, timeout=15)
    conn.request("MKCOL", base, body=MKCOL_BODY,
                 headers={"Content-Type": "application/xml",
                          "Authorization": _auth_header()})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    if resp.status not in (200, 201, 204):
        raise RuntimeError("MKCOL %s -> %d %s" % (base, resp.status, data[:80]))
    return resp.status


def propfind(host, port, base):
    conn = http.client.HTTPConnection(host, port, timeout=15)
    conn.request("PROPFIND", base, body=PROPFIND_BODY,
                 headers={"Content-Type": "application/xml", "Depth": "1",
                          "Authorization": _auth_header()})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp.status, data


def cmd_serve(args):
    storage = os.path.abspath(args.dir)
    os.makedirs(storage, exist_ok=True)
    cfg = os.path.join(storage, "radicale.conf")
    users = os.path.join(storage, "users")
    with open(users, "w", encoding="utf-8") as handle:
        # Plain-text htpasswd entry (htpasswd -p form): bench-only credential,
        # never reused anywhere; Basic auth still exercises the board path.
        handle.write("%s:%s\n" % (BENCH_USER, BENCH_PASS))
    with open(cfg, "w", encoding="utf-8") as handle:
        handle.write("[storage]\nfilesystem_folder = %s\n" %
                     os.path.join(storage, "collections").replace("\\", "/"))
        handle.write("[auth]\ntype = htpasswd\nhtpasswd_filename = %s\n" %
                     users.replace("\\", "/"))
        handle.write("htpasswd_encryption = plain\n")
        handle.write("[rights]\ntype = owner_only\n")
    print("fixture storage: %s" % storage)
    log_path = os.path.join(storage, "radicale.log")
    log_handle = open(log_path, "ab", buffering=0)
    proc = subprocess.Popen(
        [sys.executable, "-m", "radicale", "--config", cfg,
         "--hosts", "0.0.0.0:%d" % args.port],
        stdout=log_handle, stderr=subprocess.STDOUT)
    print("server log: %s" % log_path)
    try:
        base = "/alice/contacts/"
        for _ in range(75):
            try:
                try:
                    mkcol("127.0.0.1", args.port, base)
                except RuntimeError as exc:
                    # Collection already exists from a previous run.
                    if "405" not in str(exc) and "409" not in str(exc):
                        raise
                put_card("127.0.0.1", args.port, base, "probe.vcf",
                         PROBE_VCARD)
                break
            except OSError:
                # Server not up yet.
                time.sleep(0.4)
        else:
            print("FAIL: radicale did not come up")
            return 1
        for name, body in CONTACTS.items():
            put_card("127.0.0.1", args.port, base, name, body)
        try:
            conn = http.client.HTTPConnection("127.0.0.1", args.port, timeout=10)
            conn.request("DELETE", base + "probe.vcf",
                         headers={"Authorization": _auth_header()})
            conn.getresponse().read()
            conn.close()
        except OSError:
            pass
        status, data = propfind("127.0.0.1", args.port, base)
        n = data.count(b"<response>") + data.count(b":response>")
        print("serving %d contact(s), PROPFIND %d, %d response(s)" %
              (len(CONTACTS), status, n))
        print("board: pim net sync http://<PC-IP>:%d%s alice addressbook /p:<pass>" %
              (args.port, base))
        proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()
    return 0


def cmd_check(args):
    status, data = propfind("127.0.0.1", args.port, "/alice/contacts/")
    n = data.count(b"<response>") + data.count(b":response>")
    hrefs = data.count(b"<href>") + data.count(b":href>")
    print("PROPFIND %d, responses=%d hrefs=%d" % (status, n, hrefs))
    ok = status == 207 and n >= 1 and hrefs >= 1
    print("FIXTURE " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="Local CardDAV fixture for pim net E2E")
    sub = parser.add_subparsers(dest="verb", required=True)
    serve = sub.add_parser("serve")
    serve.add_argument("--port", type=int, default=5232)
    serve.add_argument("--dir", default=os.path.join(tempfile.gettempdir(), "p4radicale"))
    check = sub.add_parser("check")
    check.add_argument("--port", type=int, default=5232)
    args = parser.parse_args(argv)
    if args.verb == "serve":
        return cmd_serve(args)
    return cmd_check(args)


if __name__ == "__main__":
    raise SystemExit(main())
