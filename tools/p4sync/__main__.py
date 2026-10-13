# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync CLI: one-device file/screen operations over USB serial.

Usage (from the repo root):
    python tools/p4sync --port COM3 status
    python tools/p4sync --port COM3 push LOCAL REMOTE
    python tools/p4sync --port COM3 pull REMOTE [LOCAL]
    python tools/p4sync --port COM3 shot [--out shot.png]
    python tools/p4sync --port COM3 full-image --mirror DIR --db NAME [--files DIR] [--shot shot.png]

PIM merge/push uses `tools/pim_sync.py` (mirror-dir sync against the live
session serial) and package install stays with the `pkg` verbs; `full-image`
is the one-session composition the desktop app will drive (handshake + PIM
sync + file provisioning + app inventory + screenshot + manifest).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from p4sync.client import P4SyncClient  # noqa: E402
import pim_sync  # noqa: E402  (mirror-dir sync engine; no new serial logic here)


def cmd_status(client: P4SyncClient, args: argparse.Namespace) -> int:
    ok, out = client.check_status()
    print(out)
    if not ok:
        print("P4Sync handshake mismatch")
        return 1
    print("board: %s" % client.board_id())
    return 0


def cmd_push(client: P4SyncClient, args: argparse.Namespace) -> int:
    with open(args.local, "rb") as handle:
        client.push_file(args.remote, handle.read())
    print("pushed %s -> %s" % (args.local, args.remote))
    return 0


def cmd_pull(client: P4SyncClient, args: argparse.Namespace) -> int:
    data = client.pull_file(args.remote)
    if args.local:
        with open(args.local, "wb") as handle:
            handle.write(data)
        print("pulled %s -> %s (%d bytes)" % (args.remote, args.local, len(data)))
    else:
        sys.stdout.buffer.write(data)
    return 0


def cmd_shot(client: P4SyncClient, args: argparse.Namespace) -> int:
    shot = client.screenshot(args.out)
    print("shot %dx%d%s" % (shot.width, shot.height,
                            " -> %s" % args.out if args.out else ""))
    return 0


def cmd_full_image(client: P4SyncClient, args: argparse.Namespace) -> int:
    """One session: handshake + PIM sync + file provisioning + app inventory
    + screenshot + manifest. Returns nonzero on the first failing stage."""
    manifest = {
        "board": "",
        "time": int(time.time()),
        "pim": {},
        "files": [],
        "apps": {},
        "shot": None,
    }
    failures = 0

    ok, out = client.check_status()
    print(out)
    if not ok:
        print("P4Sync handshake mismatch")
        return 1
    manifest["board"] = client.board_id()
    print("board: %s" % manifest["board"])

    # PIM sync (contacts + alarms) through the pim_sync engine on the live
    # session serial. The engine does its own marker sync + retries; the
    # session prompt state is untouched (prompt-based Device.run resumes).
    ser = client.dev.session.ser
    if ser is None:
        print("full-image: no live serial session")
        return 1
    for kind, name, sub in (("vcf", args.db, "contacts"),
                            ("ics", "alarms", "alarms")):
        mirror = os.path.join(args.mirror, sub)
        os.makedirs(mirror, exist_ok=True)
        rc = pim_sync.cmd_sync(ser, kind, name, mirror)
        manifest["pim"][sub] = {"rc": rc, "mirror": mirror}
        if rc != 0:
            print("full-image: PIM %s sync failed" % sub)
            failures += 1

    # File provisioning: every file under --files lands at /sdcard/<rel>.
    if args.files:
        for root, _dirs, names in os.walk(args.files):
            for fname in sorted(names):
                local = os.path.join(root, fname)
                rel = os.path.relpath(local, args.files).replace(os.sep, "/")
                remote = "/sdcard/" + rel
                try:
                    client.push_local(local, remote)
                except Exception as exc:  # noqa: BLE001
                    print("full-image: push %s failed (%s)" % (rel, exc))
                    failures += 1
                    continue
                manifest["files"].append({"local": local, "remote": remote})
        print("full-image: provisioned %d file(s)" % len(manifest["files"]))

    # App inventory (existing verbs only; install stays with `pkg`).
    try:
        manifest["apps"]["apps"] = client.dev.run("apps", timeout=30)
        manifest["apps"]["launch"] = client.dev.run("launch /list", timeout=30)
    except Exception as exc:  # noqa: BLE001
        print("full-image: app inventory failed (%s)" % exc)
        failures += 1

    # Screenshot proof.
    try:
        shot = client.screenshot(args.shot)
        manifest["shot"] = args.shot
        print("shot %dx%d%s" % (shot.width, shot.height,
                                " -> %s" % args.shot if args.shot else ""))
    except Exception as exc:  # noqa: BLE001
        print("full-image: screenshot failed (%s)" % exc)
        failures += 1

    manifest_path = os.path.join(args.mirror, "image.json")
    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
    print("full-image: manifest -> %s (%s)" %
          (manifest_path, "CLEAN" if failures == 0 else "%d failure(s)" % failures))
    return 1 if failures else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="P4Sync USB prototype")
    parser.add_argument("--port", default=None)
    parser.add_argument("--quick", action="store_true")
    sub = parser.add_subparsers(dest="verb", required=True)
    sub.add_parser("status")
    push = sub.add_parser("push")
    push.add_argument("local")
    push.add_argument("remote")
    pull = sub.add_parser("pull")
    pull.add_argument("remote")
    pull.add_argument("local", nargs="?")
    shot = sub.add_parser("shot")
    shot.add_argument("--out", default=None)
    full = sub.add_parser("full-image")
    full.add_argument("--mirror", required=True,
                      help="host dir receiving the PIM mirrors + image.json")
    full.add_argument("--db", required=True,
                      help="contacts db name for the PIM sync stage")
    full.add_argument("--files", default=None,
                      help="host dir provisioned to /sdcard/<rel>")
    full.add_argument("--shot", default=None,
                      help="screenshot PNG path (inside or outside --mirror)")
    args = parser.parse_args(argv)

    with P4SyncClient(args.port, quick=args.quick) as client:
        if args.verb == "status":
            return cmd_status(client, args)
        if args.verb == "push":
            return cmd_push(client, args)
        if args.verb == "pull":
            return cmd_pull(client, args)
        if args.verb == "shot":
            return cmd_shot(client, args)
        if args.verb == "full-image":
            return cmd_full_image(client, args)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
