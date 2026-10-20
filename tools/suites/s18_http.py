# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""httpget feature suite: headers, POST, streaming and the cookie jar.

The network legs run against the board's OWN ``httpd``, reached over loopback
(``127.0.0.1``) so a weak bench AP cannot drop a transfer mid-body. Wi-Fi is
still required to *start* the server, which is the only step that polls the
link. ``httpd`` accepts only GET (a POST therefore exercises the 405 error
path) and does not emit ``Set-Cookie``; the cookie header parser is covered
headlessly in ``test/main/test_httpget.c``. What this suite proves on hardware
is the command surface: option parsing, ``/h:`` headers, ``/stream`` to the
transcript and to SD, ``/post`` rejection, and the ``/cookie`` show/clear/persist
path.

Files are pushed BEFORE the first HTTP request and removed afterwards, keeping
the transfer off any open-socket window (the BSP FAT mount allows few files).
"""

import re
import time

from p4test import Checklist

NAME = "http"
TAGS = ["network", "http"]

IP_RE = re.compile(r"(\d{1,3}(?:\.\d{1,3}){3})")
HTTPD_AUTH = "admin:p4mini"

FIXTURE = b"HTTPGET-STREAM-FIXTURE-0123456789\n" * 8
STREAM_OUT = "HTTPOUT.TXT"


def _ip_from(dev):
    """The station address from ipconfig, ignoring DNS lines and '-'.

    `ipconfig` lists DNS servers as bare dotted quads too, so matching the
    first dotted quad would return `100.100.1.1` while the station has no
    address. Only an `ipv4:` line that is not '-' counts.
    """
    out = dev.run("ipconfig", timeout=15)
    for line in out.splitlines():
        if "ipv4:" not in line.lower():
            continue
        m = IP_RE.search(line)
        if m and not m.group(1).startswith("0.") and m.group(1) != "255.255.255.255":
            return m.group(1)
    return ""


def _connected_ip(dev, timeout_s=150):
    """Wait for two consecutive connected+address samples.

    The bench AP is weak (RSSI around -80 dBm) and the Wi-Fi watchdog already
    retries with exponential backoff, so association can take 30-60 s. Do NOT
    re-issue `wifi connect` while waiting: that resets the backoff and aborts an
    association in progress. Requiring two samples rules out a single stale
    ``connected: yes``.
    """
    deadline = time.time() + timeout_s
    streak = 0
    while time.time() < deadline:
        status = dev.run("wifi status", timeout=15)
        if "connected: yes" in status:
            ip = _ip_from(dev)
            if ip:
                streak += 1
                if streak >= 2:
                    return ip
            else:
                streak = 0
        else:
            streak = 0
        time.sleep(1.0)
    return ""


def _ensure_httpd(dev, ssid, password):
    """Start a fresh httpd, reconnecting as needed; return True when running.

    A stop first drops any sessions left by an earlier (possibly aborted) run:
    the server allows only P4_CONFIG_HTTPD_MAX_OPEN_SOCKETS and a handful of
    stale connections otherwise makes `accept` fail with ENFILE.
    """
    # Do NOT stop first: httpd autostarts at Wi-Fi-up while the internal heap
    # still has an 8 KB block; stopping and re-creating the task later (after
    # the suites have churned the heap) can fail with ESP_ERR_HTTPD_TASK.
    # `httpd start` is idempotent, and the per-leg delays reap stale sessions.
    for _ in range(4):
        dev.run("httpd start", timeout=20)
        out = dev.run("httpd status", timeout=20)
        if "state: running" in out:
            return True
        dev.run("wifi connect %s %s" % (ssid, password), timeout=45, settle=2.0)
        _connected_ip(dev, timeout_s=20)
    return False


def _run_maybe(dev, c, label, cmd, timeout=45):
    try:
        return dev.run(cmd, timeout=timeout)
    except Exception as exc:  # noqa: BLE001
        c.check(label, False, "%r did not return: %s" % (cmd, exc))
        return ""


def _fetch(dev, cmd, want, timeout=45):
    """Run a fetch, retrying once when the body is missing.

    The EV board's internal heap is tight with the C6 up; the first request in
    a burst can be reset while the transcript/LVGL spans are being trimmed.
    A short gap lets the heap recover; a real break still fails both attempts.
    """
    out = dev.run(cmd, timeout=timeout)
    if want not in out:
        dev.run("delay 2000", timeout=15)
        out += dev.run(cmd, timeout=timeout)
    return out


def run(dev, ctx):
    c = Checklist(NAME)

    # -- offline option surface --------------------------------------------
    out = dev.run("httpget", timeout=15)
    c.expect("bare httpget prints usage", "Usage: httpget", out)

    out = dev.run("httpget /nope http://127.0.0.1/", timeout=15)
    c.expect("unknown option refused", "unknown option", out)

    out = dev.run("httpget /post", timeout=15)
    c.expect("post without a field refused", "/post expects NAME=VALUE", out)

    out = dev.run("httpget /h:NOEQUALS http://127.0.0.1/", timeout=15)
    c.expect("header without '=' refused", "/h: expects", out)

    out = dev.run("httpget /cookie", timeout=15)
    c.check("cookie show responds", "no stored cookies" in out or "=" in out,
            out[-200:])

    out = dev.run("httpget /cookie clear", timeout=15)
    c.expect("cookie clear responds", "cookie jar cleared", out)

    ssid, password = (ctx.get("wifi") or (None, None))
    if not ssid:
        c.note("no tools/wifi_local.json credentials; live HTTP legs skipped")
        return c

    # -- bring the link up, then run every transfer over loopback -----------
    dev.run("wifi connect %s %s" % (ssid, password), timeout=45, settle=2.0)
    ip = _connected_ip(dev)
    if not ip:
        # One clean retry in case the station was wedged from an earlier run.
        dev.run("wifi disconnect", timeout=20)
        dev.run("wifi connect %s %s" % (ssid, password), timeout=45, settle=2.0)
        ip = _connected_ip(dev)
    if not ip:
        # The bench AP is external and can be off/overloaded. The offline option
        # checks above are still graded; the live HTTP legs need the link, and
        # s11_connectivity is the suite that gates Wi-Fi itself.
        c.note("no IP from ipconfig; live HTTP legs skipped")
        return c

    dev.push_file("HTTPFIX.TXT", FIXTURE)
    c.check("httpd started", _ensure_httpd(dev, ssid, password),
            "httpd did not reach state: running")
    if "state: running" not in dev.run("httpd status", timeout=20):
        return c

    # Loopback keeps the transfer independent of AP RSSI.
    base = "http://%s@127.0.0.1" % HTTPD_AUTH
    dev.run("delay 500", timeout=15)

    out = _fetch(dev, "httpget %s/HTTPFIX.TXT" % base, "HTTPGET-STREAM-FIXTURE")
    c.expect("plain GET succeeds", "Failed to fetch URL", out, want=False)
    c.expect("plain GET body printed", "HTTPGET-STREAM-FIXTURE", out)

    # Let the server reap the just-closed session before opening the next one
    # (max_open_sockets=4); sequential requests without a gap can hit ENFILE.
    dev.run("delay 1000", timeout=15)
    out = _fetch(dev, "httpget /h:X-P4Test=bench %s/HTTPFIX.TXT" % base,
                 "HTTPGET-STREAM-FIXTURE")
    c.expect("extra header GET succeeds", "Failed to fetch URL", out, want=False)
    c.expect("extra header GET body printed", "HTTPGET-STREAM-FIXTURE", out)

    dev.run("delay 1000", timeout=15)
    out = _fetch(dev, "httpget /stream %s/HTTPFIX.TXT" % base,
                 "HTTPGET-STREAM-FIXTURE")
    c.expect("stream to transcript", "HTTPGET-STREAM-FIXTURE", out)

    dev.run("delay 1000", timeout=15)
    out = _fetch(dev, "httpget /stream sd:/%s %s/HTTPFIX.TXT" % (STREAM_OUT, base),
                 "streamed to")
    c.expect("stream to file reports success", "streamed to", out)
    out = dev.run("type %s" % STREAM_OUT, timeout=15)
    c.expect("streamed file has the body", "HTTPGET-STREAM-FIXTURE", out)
    dev.run("del /f %s" % STREAM_OUT, timeout=30)

    dev.run("delay 1500", timeout=15)
    out = _fetch(dev, "httpget /post a=b %s/" % base, "405")
    c.expect("POST reaches the server (405 is expected)", "405", out)

    # Cookie jar persistence: clearing writes the file, showing it stays empty.
    out = dev.run("httpget /cookie clear", timeout=15)
    c.expect("cookie clear ok", "cookie jar cleared", out)
    out = dev.run("dir HTTPCOOK.TXT", timeout=15)
    c.check("cookie file exists after clear", "HTTPCOOK.TXT" in out, out[-200:])
    out = dev.run("httpget /cookie", timeout=15)
    c.expect("cookie jar is empty", "no stored cookies", out)

    dev.run("del /f HTTPFIX.TXT", timeout=30)
    dev.run("del /f HTTPCOOK.TXT", timeout=30)
    dev.run("wifi disconnect", timeout=20)

    return c
