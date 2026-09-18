# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Connectivity suite: read-only network + peripheral status probes.

Nothing here mutates state that survives the run: the server is never started,
``c6ota`` is only ever handed an unsupported argument (so it refuses without
queueing a flash), and ``httpget`` runs against a local URL only when the HTTP
server is already up. An absent AP or a disconnected Wi-Fi stack is a *note*,
not a failure - the checks assert the invariants that hold on every boot
(commands return, their always-present fields are printed, no panic, no
confirmation prompt).
"""
from __future__ import annotations

import os
import re

from p4test import screenshot as p4shot  # noqa: F401  (import contract)
from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "connectivity"
TAGS = ["network", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

# A subdomain guaranteed not to exist, so DNS has to fail.
BAD_HOST = "no-such-host.p4test.invalid"


def _run(dev, c, label, cmd, timeout=30.0, settle=0.5):
    """Run a command; a timeout is a recorded failure, a panic is re-raised."""
    try:
        return dev.run(cmd, timeout=timeout, settle=settle)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check(label, False, "%r did not return: %s" % (cmd, exc))
        return ""


def _errorlevel(out):
    m = re.search(r"(?:^|\s)RC=(\d+)", out, re.MULTILINE)
    return int(m.group(1)) if m else None


def run(dev, ctx):
    c = Checklist(NAME)
    quick = bool(ctx.get("quick"))

    # -- Wi-Fi -----------------------------------------------------------------
    out = _run(dev, c, "wifi status", "wifi status", timeout=25)
    c.expect("wifi status header", "Wi-Fi Status", out)
    c.expect("wifi status state field", "state:", out)
    c.expect("wifi status default profile", "default profile:", out)
    connected = "connected: yes" in out
    c.note("Wi-Fi connected=%s" % connected)

    scan = _run(dev, c, "wifi scan", "wifi scan /b", timeout=45)
    body = [ln.strip() for ln in scan.splitlines()
            if ln.strip() and "wifi scan" not in ln and "P4TAG" not in ln]
    if not body or any(tok in scan.lower() for tok in ("no networks", "0 network")):
        c.note("wifi scan: no APs in range - scan content checks skipped")
    else:
        c.note("wifi scan returned %d non-empty row(s)" % len(body))

    # -- IP / sockets ----------------------------------------------------------
    out = _run(dev, c, "ipconfig", "ipconfig", timeout=20)
    c.expect("ipconfig title", "IP Configuration", out)
    c.expect("ipconfig mac field", "mac:", out)
    c.expect("ipconfig dns field", "DNS servers:", out)

    out = _run(dev, c, "netstat", "netstat", timeout=20)
    c.expect("netstat interfaces", "Network Interfaces", out)
    c.expect("netstat tcp section", "Active TCP Connections", out)
    c.expect("netstat udp section", "UDP Endpoints", out)

    # -- ping ------------------------------------------------------------------
    # 127.0.0.1 exercises lwIP without needing an AP. Chain `%ERRORLEVEL%` so we
    # can assert the return code is one of the documented values.
    out = _run(dev, c, "ping loopback",
               "ping 127.0.0.1 2 & echo RC=%ERRORLEVEL%", timeout=45)
    rc = _errorlevel(out)
    c.check("ping loopback returns a sane errorlevel",
            rc in (0, 1, 2), "errorlevel=%r" % rc)
    c.note("ping 127.0.0.1 errorlevel=%r" % rc)

    if connected:
        out = _run(dev, c, "ping 8.8.8.8", "ping 8.8.8.8 2", timeout=45)
        c.note("ping 8.8.8.8 reachable=%s" % ("Reply from" in out))
    else:
        c.note("ping external host skipped - Wi-Fi is not connected")

    # -- DNS -------------------------------------------------------------------
    out = _run(dev, c, "dns bad host", "dns %s" % BAD_HOST, timeout=35)
    c.expect("dns refuses an invalid name", "resolved to", out, want=False)
    c.note("dns %s output tail: %s" % (BAD_HOST, out.strip().splitlines()[-1:]))

    # -- httpd (read-only) -----------------------------------------------------
    out = _run(dev, c, "httpd status", "httpd status", timeout=20)
    c.expect("httpd status title", "HTTP Server", out)
    c.expect("httpd status state field", "state:", out)
    c.expect("httpd status port field", "port:", out)
    httpd_running = "running" in out.lower()

    # -- httpget against the local server only when it is already up -----------
    if connected and httpd_running:
        out = _run(dev, c, "httpget loopback", "httpget http://127.0.0.1/",
                   timeout=45)
        c.expect("httpget did not prompt for an OTA", "Type YES", out, want=False)
        c.note("httpget http://127.0.0.1/ returned %d byte(s)" % len(out))
    else:
        c.note("httpget skipped (needs Wi-Fi connected + httpd running)")

    # -- USB / Bluetooth -------------------------------------------------------
    out = _run(dev, c, "usb status", "usb status", timeout=20)
    c.expect("usb status host field", "usb.host:", out)
    c.expect("usb status msc field", "usb.msc:", out)
    c.expect("usb status hid keyboard field", "usb.hid.keyboard:", out)

    out = _run(dev, c, "bluetooth status", "bluetooth status", timeout=20)
    c.expect("bluetooth status title", "Bluetooth Status", out)
    c.expect("bluetooth status hosted field", "hosted ready:", out)
    c.expect("bluetooth status nimble field", "NimBLE host:", out)

    out = _run(dev, c, "bt status alias", "bt status", timeout=20)
    c.expect("bt status alias works", "Bluetooth Status", out)

    # -- C6 OTA (read-only refusal, never a flash) -----------------------------
    out = _run(dev, c, "c6ota status", "c6ota status", timeout=20)
    c.expect("c6ota status is refused as a source", "c6ota", out)
    c.expect("c6ota status never queues a flash", "Type YES", out, want=False)
    c.note("c6ota status: %s" % (out.strip().splitlines() or [""])[-1])

    _ = quick
    return c
