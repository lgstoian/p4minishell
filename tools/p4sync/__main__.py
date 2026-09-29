# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync CLI: one-device file/screen operations over USB serial.

Usage (from the repo root):
    python tools/p4sync --port COM3 status
    python tools/p4sync --port COM3 push LOCAL REMOTE
    python tools/p4sync --port COM3 pull REMOTE [LOCAL]
    python tools/p4sync --port COM3 shot [--out shot.png]

PIM sync stays with `tools/pim_sync.py` (pull/push/sync against a mirror
dir) and package install with the `pkg` verbs; this CLI only proves the
handshake plus the file/screen paths the desktop app will compose.
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from p4sync.client import P4SyncClient  # noqa: E402


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
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
