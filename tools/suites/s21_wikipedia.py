# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Wikipedia acceptance suite: the release gate for the browser round.

The pass/fail gate is deterministic and offline: a trimmed but structurally
faithful copy of https://en.wikipedia.org/wiki/ESP32 lives in
``tools/fixtures/wiki_esp32/``, is pushed to ``sd:/WIKI/`` and rendered by the
``browser`` verb. Every URL in the fixture is local, so no AP, DNS or HTTP
server is needed. The rendered TUI page is read back with ``tui dump /a``
(text + colour runs) and the screen is captured to ``spikes/wiki_esp32.bmp``.

Asserted surface: the H1 title, an infobox row, a feature-table cell, the
unordered list, a link's cyan foreground (DOS 11), image alt-text fallback for
a missing source, and no panic.

Live mode is opt-in: export ``P4_WIFI_LIVE=1`` and put the bench AP in
``tools/wifi_local.json`` (git-ignored; never hardcode credentials here). It
fetches the real HTTPS page through the cert-bundle path and accepts either a
full render or the documented size-cap degradation.
"""

import os
import re
import time

from p4test import Checklist


NAME = "wikipedia"
TAGS = ["browser", "ui", "network", "release"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURE = os.path.join(ROOT, "tools", "fixtures", "wiki_esp32")
SPIKES = os.path.join(ROOT, "spikes")

LIVE_URL = "https://en.wikipedia.org/wiki/ESP32"
LINK_TEXT = "official ESP-IDF page"
LINK_FG = 11            # bright cyan, the documented <a> foreground
MISSING_ALT = "[ESP32 die shot unavailable]"

IP_RE = re.compile(r"(\d{1,3}(?:\.\d{1,3}){3})")
TEXT_RE = re.compile(r"^(\d{3})\|(.*)$", re.MULTILINE)
ATTR_RE = re.compile(r"^(\d{3})@(.*)$", re.MULTILINE)
RUN_RE = re.compile(r"(\d+)-(\d+):(\d+)")


def _push_fixture(dev, c):
    dev.run("md WIKI", timeout=20)
    for name in ("ESP32.HTML", "PROGRAMMING.HTML",
                 "ESP32-MODULE.PNG", "ESP32-DIE.JPG"):
        dev.push_local(os.path.join(FIXTURE, name), "WIKI/" + name)
    out = dev.run("dir WIKI/ESP32.HTML", timeout=20)
    c.expect("fixture deployed to sd:/WIKI", "ESP32.HTML", out)


def _dump(dev):
    # The full logical page (P4_CONFIG_TUI_PAGE_ROWS = 200): the trimmed article
    # is ~100 rows, so a shorter window would miss the link list and the
    # missing-image alt fallback near the bottom.
    return dev.run("tui dump /a 1 200", timeout=60)


def _text_rows(dump):
    return {int(m.group(1)): m.group(2) for m in TEXT_RE.finditer(dump)}


def _attr_rows(dump):
    return {int(m.group(1)): m.group(2) for m in ATTR_RE.finditer(dump)}


def _row_with(rows, needle):
    for row in sorted(rows):
        if needle in rows[row]:
            return row, rows[row]
    return 0, ""


def _fg_run_covers(attrs, start, end, fg):
    """True when a cell run in @p attrs covers [start, end] with @p fg."""
    for m in RUN_RE.finditer(attrs):
        lo, hi, got = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if got == fg and lo <= start and hi >= end:
            return True
    return False


def _wait_ip(dev, timeout_s=120):
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


def _wait_clock(dev, timeout_s=30):
    """HTTPS certificate validation needs a sane wall clock."""
    dev.run("sntp sync", timeout=20)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        out = dev.run("date", timeout=15)
        if "NTP sync: synced" in out:
            return True
        m = re.search(r"(\d{4})-\d{2}-\d{2}", out)
        if m and int(m.group(1)) >= 2020:
            return True
        time.sleep(1.0)
    return False


def _assert_page(c, dump):
    rows = _text_rows(dump)
    attrs = _attr_rows(dump)

    # Title: the H1 is its own short row (not a substring match anywhere).
    title_rows = [r for r, text in rows.items() if text.strip() == "ESP32"]
    c.check("H1 title rendered", bool(title_rows),
            "no row is exactly 'ESP32'; rows=%r" % sorted(rows.items())[:12])

    # Infobox row: both cells on one rendered table line.
    row, text = _row_with(rows, "Manufacturer")
    c.check("infobox row rendered", bool(row) and
            "Espressif Systems" in text,
            "row=%r text=%r" % (row, text))

    # Feature table cell.
    row, text = _row_with(rows, "CPU frequency")
    c.check("feature table cell rendered", bool(row) and "240 MHz" in text,
            "row=%r text=%r" % (row, text))

    # Unordered list items.
    c.expect("unordered list rendered", "MicroPython and CircuitPython", dump)
    c.expect("unordered list second item", "Rust and Zig", dump)

    # Link colouring: the unique link text must sit in a cyan (fg 11) run.
    row, text = _row_with(rows, LINK_TEXT)
    start = text.find(LINK_TEXT) + 1
    end = start + len(LINK_TEXT) - 1
    c.check("link text rendered", bool(row), "row=%r text=%r" % (row, text))
    def _fg_at(col):
        for m in RUN_RE.finditer(attrs.get(row, "")):
            lo, hi, got = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if lo <= col <= hi:
                return got
        return None

    # CSS backgrounds show through the spaces between link words (a block cell
    # with the page colour), so require every *glyph* of the link to be cyan
    # rather than one unbroken run.
    cyan_glyphs = bool(row) and all(
        _fg_at(col) == LINK_FG
        for col in range(start, end + 1)
        if col - 1 < len(text) and text[col - 1] != " "
    )
    c.check("link is cyan (fg 11)", cyan_glyphs,
            "row=%r attrs=%r start=%d end=%d"
            % (row, attrs.get(row, ""), start, end))

    # Image alt-text fallback for the intentionally missing source.
    c.expect("missing image degrades to alt text", MISSING_ALT, dump)

    # No panic / fatal path anywhere in the render or dump.
    c.check("render and dump did not panic",
            "Guru Meditation" not in dump and "abort()" not in dump and
            "***ERROR***" not in dump,
            dump[-400:])


def _capture(dev, c):
    out = dev.run("screenshot sd:/WIKI/SHOT.BMP", timeout=90)
    c.expect("screenshot saved on SD", "screenshot: saved", out)
    try:
        data = dev.pull_file("WIKI/SHOT.BMP")
        os.makedirs(SPIKES, exist_ok=True)
        path = os.path.join(SPIKES, "wiki_esp32.bmp")
        with open(path, "wb") as f:
            f.write(data)
        c.check("screenshot is a BMP", data[:2] == b"BM" and len(data) > 1000,
                "%d bytes, magic=%r" % (len(data), data[:2]))
    except Exception as exc:  # noqa: BLE001
        c.check("screenshot captured", False, str(exc))
    dev.run("del /f WIKI/SHOT.BMP", timeout=30)


def _live(dev, c, ctx):
    """Opt-in real HTTPS fetch; recorded, not the pass/fail gate."""
    ssid, password = (ctx.get("wifi") or (None, None))
    if not ssid:
        c.note("P4_WIFI_LIVE=1 but tools/wifi_local.json has no credentials; "
               "live leg skipped")
        return
    dev.run("wifi connect %s %s" % (ssid, password), timeout=45, settle=3.0)
    ip = _wait_ip(dev)
    c.note("live Wi-Fi ip: %s" % (ip or "none"))
    if not _wait_clock(dev):
        c.check("live clock ready for TLS", False,
                "clock did not synchronize; HTTPS certificate validation would fail")
        dev.run("wifi disconnect", timeout=20)
        dev.run("wifi forget", timeout=15)
        return

    dev.run("browser cache clear", timeout=20)
    out = dev.run("browser %s" % LIVE_URL, timeout=240, settle=3.0)
    dump = _dump(dev)

    rendered = ("Failed to" not in out and "Failed to fetch" not in out and
                "Espressif" in dump and "ESP32" in dump)
    if rendered:
        c.check("live HTTPS full render", "Espressif" in dump,
                "Wikipedia rendered through the cert-bundle path")
        c.expect("live render shows the infobox", "Espressif Systems", dump)
    else:
        # The transport rejects a page above P4_CONFIG_HTTP_MAX_BODY_BYTES
        # instead of exhausting PSRAM. That is the documented degradation.
        degraded = ("Page exceeds the" in out or "exceeds the" in out)
        c.check("live fetch bounded by the documented size cap", degraded,
                out[-500:])
        c.check("live degradation did not crash the board",
                "Guru Meditation" not in out + dump, out[-300:])

    if ssid:
        dev.run("wifi disconnect", timeout=20)
        dev.run("wifi forget", timeout=15)


def run(dev, ctx):
    c = Checklist(NAME)

    _push_fixture(dev, c)
    dev.run("browser cache clear", timeout=20)

    out = dev.run("browser sd:/WIKI/ESP32.HTML", timeout=120, settle=3.0)
    c.check("offline fixture rendered", "Failed to" not in out,
            out[-500:])

    # Capture before the dump: the transcript append from `tui dump` can repaint
    # the shell over the TUI page, while the screenshot must show the page.
    _capture(dev, c)
    dump = _dump(dev)
    _assert_page(c, dump)

    if os.environ.get("P4_WIFI_LIVE") == "1":
        try:
            _live(dev, c, ctx)
        except Exception as exc:  # noqa: BLE001
            c.check("live Wikipedia leg completed", False, str(exc))
    else:
        c.note("live Wikipedia leg skipped (set P4_WIFI_LIVE=1 to record it)")

    return c
