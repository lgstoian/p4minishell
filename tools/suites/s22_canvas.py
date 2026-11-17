# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Canvas 2D + CSS background acceptance suite.

Renders `apps/website/canvas.html` with the on-device browser. The page uses
the stripped-down WHATWG canvas subset (fillStyle/fillRect/font/fillText) and
CSS Backgrounds 3 colours for the page and a block. Assertions cover:

- the canvas width/height attributes reached the JS context and the bitmap;
- the canvas pixels reached the TUI grid (colour runs for bright red, green
  and cyan, which are exact DOS palette entries);
- CSS backgrounds are painted as block cells in the page and banner colours;
- the page did not panic and a screenshot was captured.

No network, AP or JS host is needed: it is the deterministic gate for the HTML5
subset, while the live Wikipedia suite remains the network gate.
"""

import os

from p4test import Checklist

NAME = "canvas"
TAGS = ["browser", "ui", "html5"]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
SPIKES = os.path.join(ROOT, "spikes")

# Exact DOS palette entries used by the page and the canvas.
PAGE_BG = (0, 170, 170)      # #00aaaa -> DOS 3
BANNER_BG = (170, 0, 0)      # #aa0000 -> DOS 4
CANVAS_RED = (255, 85, 85)   # #ff5555 -> DOS 12
CANVAS_GREEN = (85, 255, 85) # #55ff55 -> DOS 10
CANVAS_CYAN = (85, 255, 255) # #55ffff -> DOS 11


def _push(dev, name, remote):
    with open(os.path.join(APPS, "website", name), "rb") as fh:
        dev.push_file(remote, fh.read())


def _count_near(bmp, rgb, tol=10, step=3):
    """Sample pixels and count those within @p tol of @p rgb (pure Python, so
    the suite does not depend on numpy being importable on the host)."""
    n = 0
    for y in range(0, bmp.height, step):
        for x in range(0, bmp.width, step):
            r, g, b = bmp.px(x, y)
            if abs(r - rgb[0]) <= tol and abs(g - rgb[1]) <= tol and \
                    abs(b - rgb[2]) <= tol:
                n += 1
    return n


def run(dev, ctx):
    c = Checklist(NAME)
    dev.run("md WEB", timeout=20)
    _push(dev, "canvas.html", "WEB/CANVAS.HTML")

    dev.run("browser cache clear", timeout=20)
    out = dev.run("browser sd:/WEB/CANVAS.HTML", timeout=90, settle=2.0)
    dump = dev.run("tui dump 1 25", timeout=30)
    attrs = dev.run("tui dump 1 25 /a", timeout=30)

    c.expect("page title rendered", "Canvas 2D + CSS backgrounds", dump)
    c.expect("canvas attributes reached the context", "CW=96x48", dump)
    c.check("canvas bitmap replaced the fallback content",
            "Canvas fallback" not in dump, "fallback text still present")
    c.check("no panic during render", "Guru Meditation" not in out + dump,
            "panic marker" if "Guru Meditation" in out + dump else "")

    # The canvas is drawn through the shared image path: its bright red,
    # green and cyan quadrants appear as foreground colour runs.
    for label, code in (("red", ":12"), ("green", ":10"), ("cyan", ":11")):
        c.check("canvas %s run rendered" % label, code in attrs,
                "missing %s in colour-runs dump" % code)

    bmp = dev.screenshot()
    for label, rgb, minimum in (
        ("page background", PAGE_BG, 60),
        ("banner background", BANNER_BG, 12),
        ("canvas red", CANVAS_RED, 12),
        ("canvas green", CANVAS_GREEN, 12),
        ("canvas cyan", CANVAS_CYAN, 12),
    ):
        n = _count_near(bmp, rgb)
        c.check("%s pixels visible" % label, n >= minimum, "%d samples" % n)

    # CSS background-image + an inline PNG. Regression: LVGL's vendored
    # LodePNG returns an lv_draw_buf_t (not raw RGBA), so treating the pointer
    # as pixels turned every PNG white; a wrong channel order tinted a blue
    # background orange. The fixture is 16x16 #ff5555 (DOS red 12).
    _push(dev, "PIX.PNG", "WEB/PIX.PNG")
    _push(dev, "BGIMG.HTML", "WEB/BGIMG.HTML")
    dev.run("browser cache clear", timeout=20)
    out = dev.run("browser sd:/WEB/BGIMG.HTML", timeout=90, settle=2.0)
    dump = dev.run("tui dump 1 12", timeout=30)
    c.expect("PNG background page rendered", "PNG BACKDROP", dump)
    c.check("no panic during the PNG render",
            "Guru Meditation" not in out + dump, "panic marker")
    bmp = dev.screenshot()
    red = _count_near(bmp, CANVAS_RED)
    c.check("PNG background decodes to red (not white)", red >= 120,
            "%d red samples" % red)
    # The inline <img> is cell art, so it lands as red glyph cells: a white or
    # mis-tinted decode would leave the whole page outside the red family.
    c.check("PNG background tiles the page", red >= 400,
            "%d red samples (tiled backdrop)" % red)

    try:
        os.makedirs(SPIKES, exist_ok=True)
        with open(os.path.join(SPIKES, "canvas_page.bmp"), "wb") as fh:
            fh.write(bmp.data)
        c.check("canvas screenshot captured", len(bmp.data) > 1000,
                "%d bytes" % len(bmp.data))
    except OSError as exc:  # noqa: BLE001 - a missing spike dir is a failure
        c.check("canvas screenshot captured", False, str(exc))

    return c
