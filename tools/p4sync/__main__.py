# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync CLI: host-side file/screen/shell operations over USB serial.

Usage (from the repo root):
    python tools/p4sync --port COM3 status
    python tools/p4sync --port COM3 push LOCAL REMOTE
    python tools/p4sync --port COM3 pull REMOTE [LOCAL]
    python tools/p4sync --port COM3 shot [--out shot.png]
    python tools/p4sync --port COM3 full-image --mirror DIR --db NAME [--files DIR] [--shot shot.png]

    # every bridge command is also a CLI subcommand:
    python tools/p4sync --mock devices
    python tools/p4sync --board jc1060p470c --json status
    python tools/p4sync --port COM3 shell "dir /b /s /sdcard"
    python tools/p4sync --port COM3 screenshot --out shot.png
    python tools/p4sync --port COM3 verify REMOTE --crc32 1a2b3c4d
    python tools/p4sync --port COM3 plan LOCAL /sdcard --dry-run
    python tools/p4sync --port COM3 pkg list

`--json` prints exactly the bridge `data` object (one JSON object on stdout);
progress and errors go to stderr. `--mock` needs no board. All device I/O uses
the shared p4test harness; there is no second serial/framing implementation.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from p4sync import protocol as P  # noqa: E402
from p4sync.bridge import Bridge, BridgeError  # noqa: E402  (one implementation)

# board_ports, pim_sync and P4SyncClient import pyserial through the shared
# harness. They are imported inside the paths that actually need a real board,
# so `--mock` runs on a machine with no pyserial installed.


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


def cmd_full_image(client, args: argparse.Namespace) -> int:
    """One session: handshake + PIM sync + file provisioning + app inventory
    + screenshot + manifest. Returns nonzero on the first failing stage."""
    import pim_sync  # lazy: existing PIMX merge engine, needs the harness
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


# ---------------------------------------------------------------------------
# Bridge-backed subcommands (same data schema as the GUI bridge)
# ---------------------------------------------------------------------------

LEGACY = ("status", "push", "pull", "shot", "full-image")
NEW_COMMANDS = ("devices", "shell", "screenshot", "verify", "plan", "pkg")


def resolve_port(args: argparse.Namespace) -> Optional[str]:
    """--port wins, then --board (existing resolver), then the env default."""
    if getattr(args, "port", None):
        return str(args.port).upper()
    if args.mock:
        return "MOCK1"
    board = getattr(args, "board", None)
    if board:
        import board_ports  # lazy: needs pyserial
        return board_ports.resolve_port(board)
    if args.mock:
        return "MOCK1"
    return os.environ.get("P4SYNC_PORT")


def build_request(args: argparse.Namespace, verb: str) -> dict:
    port = resolve_port(args)
    cmd = "screenshot" if verb == "shot" else verb
    req: dict = {"id": 0, "cmd": cmd,
                 "quick": bool(getattr(args, "quick", False))}
    if port:
        req["port"] = port
    if args.timeout is not None:
        req["timeout"] = args.timeout
    if verb == "shell":
        req.update({"command": args.command, "settle": args.settle,
                    "plain": bool(args.plain)})
    elif verb in ("push", "pull", "screenshot", "shot", "verify"):
        if verb == "push":
            req.update({"local": args.local, "remote": args.remote})
        elif verb == "pull":
            req.update({"remote": args.remote, "local": args.local})
        elif verb in ("screenshot", "shot"):
            req["out"] = args.out
        elif verb == "verify":
            req.update({"remote": args.remote, "crc32": args.crc32})
    elif verb == "full-image":
        req.update({"mirror": args.mirror, "db": args.db,
                    "files": args.files, "shot": args.shot})
    elif verb == "plan":
        req.update({"local": args.local, "remote": args.remote,
                    "cache": args.cache, "dry_run": not args.apply,
                    "apply": bool(args.apply)})
    elif verb == "pkg":
        req.update({"verb": args.pkg_verb, "package": args.package})
    return req


def human_summary(verb: str, data: dict) -> None:
    """Compact human rendering; --json callers get the raw data object."""
    if verb == "devices":
        for device in data.get("devices", []):
            print("%s  %s  %s" % (device["port"], device["board"] or "-",
                                  device["status"]))
    elif verb == "status":
        print("board: %s  proto: %s  sd: %s  lock: %s  display: %s"
              % (data["board"], data["proto"], data["sd"], data["lock"],
                 "x".join(str(v) for v in data["display"])))
    elif verb in ("shell", "pkg"):
        print(data.get("output", ""), end="" if data.get("output", "").endswith("\n") else "\n")
        print("ERRORLEVEL: %s" % data.get("errorlevel"))
    elif verb in ("screenshot", "shot"):
        proof = (" sha256=%s bytes=%d" % (data["sha256"], data["size"])
                 if "sha256" in data else "")
        print("shot %dx%d -> %s%s"
              % (data["width"], data["height"], data["out"], proof))
    elif verb == "push":
        print("pushed %s -> %s (%d bytes, crc32 %s, .part verified)"
              % (data["local"], data["remote"], data["bytes"], data["crc32"]))
    elif verb == "pull":
        print("pulled %s -> %s (%d bytes, crc32 %s)"
              % (data["remote"], data["local"], data["bytes"], data["crc32"]))
    elif verb == "verify":
        print("verify %s: %s (crc32 %s)"
              % (data["remote"], "OK" if data["ok"] else "MISMATCH",
                 data["crc32"]))
    elif verb == "plan":
        s = data["summary"]
        print("plan: %d file(s), %d push, %d pull, %d skip, %d conflict(s), %s"
              % (s["files"], s["push"], s["pull"], s["skip"], s["conflicts"],
                 "dry-run" if s["dry_run"] else "applied %d" % s["applied"]))
        for action in data["actions"]:
            print("  %-5s %-24s %s%s" % (action["op"], action["path"],
                                         action["reason"],
                                         " CONFLICT" if action.get("conflict") else ""))
    else:
        print(json.dumps(data, indent=2))


def run_bridge_command(args: argparse.Namespace, verb: str) -> int:
    bridge = Bridge(mock=args.mock, timeout=args.timeout)
    bridge.human = True  # stdout stays a single JSON object / one human report
    req = build_request(args, verb)
    try:
        handler = Bridge.COMMANDS[req["cmd"]]
        data = handler(bridge, req, 0)
    except BridgeError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001
        print("%s: %s" % (P.error_code(exc), exc), file=sys.stderr)
        return 1
    finally:
        bridge.close()
    if args.json:
        print(json.dumps(data, ensure_ascii=False, default=str))
    else:
        human_summary(verb, data)
    # A command whose own verdict is false (failed full-image, CRC mismatch,
    # nonzero ERRORLEVEL) is a failure for scripts even though the bridge
    # transport itself completed and returned a well-formed data object.
    return 1 if data.get("ok") is False else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="P4Sync host CLI (USB serial or --mock fixtures)",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=None)
    parser.add_argument("--board", default=None,
                        help="board slug resolved via tools/board_ports.py")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--mock", action="store_true",
                        help="deterministic fixtures; no board required")
    parser.add_argument("--json", action="store_true",
                        help="print the bridge data object as one JSON line")
    parser.add_argument("--timeout", type=float, default=None,
                        help="default device timeout (seconds)")
    parser.add_argument("--settle", type=float, default=0.2,
                        help="Device.run settle seconds (shell)")
    sub = parser.add_subparsers(dest="verb", required=True)

    sub.add_parser("devices", help="list ports / mock devices")
    sub.add_parser("status", help="sync status handshake + capabilities")
    push = sub.add_parser("push")
    push.add_argument("local")
    push.add_argument("remote")
    pull = sub.add_parser("pull")
    pull.add_argument("remote")
    pull.add_argument("local", nargs="?")
    shot = sub.add_parser("shot", help="legacy alias of screenshot")
    shot.add_argument("--out", default=None)
    shell = sub.add_parser("shell", help="one existing firmware verb")
    shell.add_argument("command")
    shell.add_argument("--plain", action="store_true",
                       help="do not read ERRORLEVEL after the command")
    for name in ("screenshot",):
        p = sub.add_parser(name)
        p.add_argument("--out", default=None)
    verify = sub.add_parser("verify")
    verify.add_argument("remote")
    verify.add_argument("--crc32", required=True)
    full = sub.add_parser("full-image")
    full.add_argument("--mirror", required=True,
                      help="host dir receiving the PIM mirrors + image.json")
    full.add_argument("--db", required=True,
                      help="contacts db name for the PIM sync stage")
    full.add_argument("--files", default=None,
                      help="host dir provisioned to /sdcard/<rel>")
    full.add_argument("--shot", default=None,
                      help="screenshot PNG path (inside or outside --mirror)")
    plan = sub.add_parser("plan")
    plan.add_argument("local", help="host directory to sync from")
    plan.add_argument("remote", nargs="?", default="/sdcard")
    plan.add_argument("--cache", default=None,
                      help="host JSON cache of (path,size,mtime,crc32)")
    plan.add_argument("--dry-run", action="store_true", default=True,
                      help="only print the plan (default)")
    plan.add_argument("--apply", action="store_true",
                      help="execute the plan one file at a time")
    pkg = sub.add_parser("pkg")
    pkg.add_argument("pkg_verb", metavar="verb",
                     choices=sorted(("list", "install", "verify", "uninstall",
                                     "update", "info", "check", "purge",
                                     "outdated", "unpack")))
    pkg.add_argument("package", nargs="?")
    args = parser.parse_args(argv)

    verb = args.verb
    # Preserve the prototype's exact human output on a real board. --json,
    # --mock and all new verbs go through the bridge-backed implementation.
    legacy_real = (verb in LEGACY and not args.json and not args.mock)
    if legacy_real:
        port = resolve_port(args)
        if not port:
            print("error: no port (use --port, --board or P4SYNC_PORT)",
                  file=sys.stderr)
            return 1
        try:
            from p4sync.client import P4SyncClient  # lazy: needs pyserial
            with P4SyncClient(port, quick=args.quick) as client:
                if verb == "status":
                    return cmd_status(client, args)
                if verb == "push":
                    return cmd_push(client, args)
                if verb == "pull":
                    return cmd_pull(client, args)
                if verb == "shot":
                    return cmd_shot(client, args)
                if verb == "full-image":
                    return cmd_full_image(client, args)
        except Exception as exc:  # noqa: BLE001
            print("%s: %s" % (P.error_code(exc), exc), file=sys.stderr)
            return 1
        return 2
    if verb == "pull" and args.json and not args.local:
        print("error: --json pull needs a local destination path",
              file=sys.stderr)
        return 1
    return run_bridge_command(args, verb)


if __name__ == "__main__":
    raise SystemExit(main())
