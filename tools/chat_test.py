#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Two-board `chat` integration driver (focused; the suite runner is single-board).

Both boards join the same known AP (``tools/wifi_local.json``), discover each
other's IP with ``ipconfig``, then exchange messages over the C6 via the `chat`
verbs. SoftAP (`chat host on`) is deliberately NOT used: an always-on AP browns
out USB-powered boards (bugs.md F35). Skipped cleanly (exit 0 with a NOTE) when
no credentials are configured or the boards are not on a shared network.

Usage:
    python tools/chat_test.py --host COM3 --peer COM6
    python tools/chat_test.py COM3 COM6
"""
from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p4test import Device  # noqa: E402
from p4test.local_config import wifi_credentials  # noqa: E402

IP_RE = re.compile(r"(\d{1,3}(?:\.\d{1,3}){3})")


def _resolve_port(arg: str) -> str:
    if arg.upper().startswith("COM"):
        return arg.upper()
    import board_ports
    return (board_ports.resolve_port(arg) or arg).upper()


def _join(dev: Device, ssid: str, password: str) -> str:
    dev.run("wifi connect %s %s" % (ssid, password), timeout=40, settle=3.0)
    # Association + DHCP settle; ipconfig then reports the lease.
    for _ in range(10):
        out = dev.run("ipconfig", timeout=15)
        m = IP_RE.search(out)
        if m and not m.group(1).startswith("0.") and m.group(1) != "255.255.255.255":
            return m.group(1)
    return ""


def _leave(dev: Device) -> None:
    for cmd in ("chat peer off", "wifi disconnect", "wifi forget"):
        try:
            dev.run(cmd, timeout=15)
        except Exception:
            pass


def run(host_port: str, peer_port: str) -> int:
    ssid, password = wifi_credentials()
    if not ssid:
        print("NOTE: no tools/wifi_local.json credentials; skipping chat P2P test")
        return 0

    results = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               (" (%s)" % detail) if (detail and not ok) else ""))

    a = Device(host_port)
    b = Device(peer_port)
    try:
        ip_a = _join(a, ssid, password)
        ip_b = _join(b, ssid, password)
        check("host associated", bool(ip_a), ip_a)
        check("peer associated", bool(ip_b), ip_b)
        if not (ip_a and ip_b):
            return _summary(results)

        check("peer points at host",
              "peer" in b.run("chat peer %s" % ip_a, timeout=15))
        check("host points at peer",
              "peer" in a.run("chat peer %s" % ip_b, timeout=15))

        check("host sends", "sent" in a.run('chat send hello from host', timeout=20))
        poll = b.run("chat poll", timeout=20)
        check("peer receives", "hello from host" in poll, poll[-200:])
        check("peer log shows message",
              "hello from host" in b.run("chat log", timeout=20))

        check("peer replies", "sent" in b.run('chat send hi from peer', timeout=20))
        poll = a.run("chat poll", timeout=20)
        check("host receives", "hi from peer" in poll, poll[-200:])

        status = a.run("chat status", timeout=15)
        check("status names the peer", bool(IP_RE.search(status)), status[-200:])
    finally:
        _leave(a)
        _leave(b)
        a.close()
        b.close()

    return _summary(results)


def _summary(results) -> int:
    failed = sum(1 for _n, ok, _d in results if not ok)
    print("\nRESULT %s (%d/%d checks)"
          % ("OK" if failed == 0 else "FAIL", len(results) - failed, len(results)))
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="two-board chat integration driver")
    ap.add_argument("ports", nargs="*", help="host peer COM ports (or use flags)")
    ap.add_argument("--host", default=None)
    ap.add_argument("--peer", default=None)
    args = ap.parse_args()
    host = args.host or (args.ports[0] if len(args.ports) > 0 else "COM3")
    peer = args.peer or (args.ports[1] if len(args.ports) > 1 else "COM6")
    return run(_resolve_port(host), _resolve_port(peer))


if __name__ == "__main__":
    sys.exit(main())
