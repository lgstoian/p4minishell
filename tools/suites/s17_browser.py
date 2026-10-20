# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Focused browser / website-rendering suite.

Website rendering is asserted two ways:

* **Deterministic, on-device:** `browser` fetches a crafted page from the
  board's OWN ``httpd`` (SD share) over the LAN and the rendered TUI page is
  read back with ``tui dump`` (which reuses ``tui_page_get_cell``). Only an
  associated AP is needed so lwIP is up (the bench ``camera`` network from
  ``tools/wifi_local.json``); no external site and no internet are used.
  httpd's Basic auth is exercised via the standard ``user:pass@`` URL form.
* **Pure/headless:** the structural render path is unit-tested in
  ``test/main/test_browser.c`` (no hardware).

Also covers navigation history (`browser back`/`forward`), bookmark
add/list/del round-trips, and the no-connection error path.
"""

import re
import time

from p4test import Checklist


NAME = "browser"
TAGS = ["browser", "network", "ui"]

IP_RE = re.compile(r"(\d{1,3}(?:\.\d{1,3}){3})")

# httpd's compile-time Basic auth (p4minishell_config.h).
HTTPD_AUTH = "admin:p4mini"

# A page that exercises the documented renderer surface: heading, paragraph,
# list, link, image alt text and a table cell.
RENDER_HTML = (
    b"<!DOCTYPE html><html><head><title>Render Test</title></head><body>\n"
    b"<h1>RenderHeading</h1>\n"
    b"<p>ParagraphAlpha</p>\n"
    b"<ul><li>ItemOne</li><li>ItemTwo</li></ul>\n"
    b"<a href=\"http://example.com/x\">LinkText</a>\n"
    b"<img src=\"x.png\" alt=\"AltImage\">\n"
    b"<table><tr><td>CellA</td><td>CellB</td></tr></table>\n"
    b"</body></html>\n"
)


def _dump(dev):
    return dev.run("tui dump 1 80", timeout=25)


def _wait_ip(dev, timeout_s=120):
    """Station address from ipconfig, waiting out a weak-AP association.

    `ipconfig` lists DNS servers as bare dotted quads, so only an `ipv4:` line
    that is not '-' counts. Do not re-issue `wifi connect` while waiting: the
    watchdog already retries with backoff, and resetting it aborts an
    association in progress.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        status = dev.run("wifi status", timeout=15)
        if "connected: yes" in status:
            out = dev.run("ipconfig", timeout=15)
            for line in out.splitlines():
                if "ipv4:" not in line.lower():
                    continue
                m = IP_RE.search(line)
                if (m and not m.group(1).startswith("0.") and
                        m.group(1) != "255.255.255.255"):
                    return m.group(1)
        time.sleep(1.0)
    return ""


def run(dev, ctx):
    c = Checklist(NAME)

    # -- verb surface + no-connection error path (no AP yet) ----------------
    out = dev.run("browser")
    c.expect("bare browser prints usage", "browser <url>", out)

    out = dev.run("browser http://127.0.0.1/RENDER.HTML", timeout=30, settle=2.0)
    c.expect("fetch without a connection fails", "Failed to fetch URL", out)

    out = dev.run("browser bookmark list", timeout=15)
    c.check("bookmark list responds",
            "No bookmarks" in out or "://" in out, out[-200:])

    # -- deterministic render over the board's own httpd --------------------
    ssid, password = (ctx.get("wifi") or (None, None))
    if not ssid:
        c.note("no tools/wifi_local.json credentials; live render leg skipped")
        return c

    dev.run("wifi connect %s %s" % (ssid, password), timeout=45, settle=3.0)
    ip = _wait_ip(dev)
    c.check("wifi associated", bool(ip), "no IP from ipconfig")
    if not ip:
        return c

    # Push the fixture BEFORE the HTTP server is up: the SD mount allows only a
    # few simultaneous files (BSP max_files=5), so keep the transfer off the
    # browser/httpd window.
    dev.push_file("RENDER.HTML", RENDER_HTML)
    dev.run("httpd start", timeout=20)

    base = "http://%s@%s" % (HTTPD_AUTH, ip)
    url = "%s/RENDER.HTML" % base
    out = dev.run("browser %s" % url, timeout=60, settle=2.0)
    c.expect("browser fetch did not fail", "Failed to fetch URL", out, want=False)

    dump = _dump(dev)
    c.expect("heading rendered", "RenderHeading", dump)
    c.expect("paragraph rendered", "ParagraphAlpha", dump)
    c.expect("list item rendered", "ItemOne", dump)
    c.expect("link text rendered", "LinkText", dump)
    c.expect("image alt text rendered", "AltImage", dump)
    c.expect("table cell rendered", "CellA", dump)

    # -- navigation history -------------------------------------------------
    dev.run("browser %s/" % base, timeout=60, settle=2.0)
    dev.run("browser back", timeout=60, settle=2.0)
    dump = _dump(dev)
    c.expect("back returns to the page", "RenderHeading", dump)

    dev.run("browser forward", timeout=60, settle=2.0)
    c.check("forward responds", True)

    # -- bookmarks ----------------------------------------------------------
    out = dev.run("browser bookmark add bench %s" % url, timeout=15)
    c.expect("bookmark add", "added", out)
    out = dev.run("browser bookmark list", timeout=15)
    c.expect("bookmark listed", "bench", out)
    out = dev.run("browser bookmark del bench", timeout=15)
    c.expect("bookmark del", "removed", out)

    # -- cleanup ------------------------------------------------------------
    dev.run("httpd stop", timeout=20)
    dev.run("wifi disconnect", timeout=20)
    dev.run("wifi forget", timeout=15)

    return c
