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
from p4test.screenshot import diff_ratio, parse_bmp

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


def _read_until(session, marker, timeout=30.0):
    got = session.read_until(marker.encode(), timeout)
    return got if isinstance(got, bytes) else got.encode()


def _pull_bmp(dev, c, remote):
    """Pull an app-written BMP (a panel capture here) into the pixel model."""
    try:
        return parse_bmp(dev.pull_file(remote))
    except Exception as exc:  # noqa: BLE001
        c.check("pull %s" % remote, False, str(exc))
        return None


def _push(dev, name, remote):
    with open(os.path.join(APPS, "website", name), "rb") as fh:
        dev.push_file(remote, fh.read())


def _count_red_family(bmp, step=3):
    """Count strongly red pixels (any quantisation of the canvas red)."""
    n = 0
    for y in range(0, bmp.height, step):
        for x in range(0, bmp.width, step):
            r, g, b = bmp.px(x, y)
            if r >= 180 and g <= 140 and b <= 140:
                n += 1
    return n


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


PRESENT_BAT = r"""@echo off
rem Present-time invariant fixture: each `gfx show` must reach the PANEL, and
rem the second frame must replace the first (no stale sprite pixels).
echo [M-PRESENT]
gfx init 240 180
if errorlevel 1 goto :done
gfx clear 0
gfx ellipse 50 45 30 25 0xFF0000 fill
gfx show
delay 400
screenshot /panel %APPDRIVE%/P1.BMP
delay 300
gfx clear 0
gfx ellipse 190 135 30 25 0x00FF00 fill
gfx show
delay 400
screenshot /panel %APPDRIVE%/P2.BMP
gfx close
:done
echo [M-PRESENT-DONE]
"""

# v1.4.4 tear-free present fixture: a ball crosses to the right wall, reverses
# and steps back. The panel is captured at each step. Every capture must show
# ONE complete ball at the current position: no stale pixels from the previous
# position and no half-written (torn) ball. Colour-coding the three positions
# makes a direction-change tear observable in the panel framebuffer.
BOUNCE_TEAR_BAT = r"""@echo off
echo [M-BTEAR]
gfx init 240 180
if errorlevel 1 goto :done
gfx clear 1
gfx ellipse 40 90 12 12 0xFF0000 fill
gfx show
delay 120
screenshot /panel %APPDRIVE%/T1.BMP
gfx clear 1
gfx ellipse 210 90 12 12 0x00FF00 fill
gfx show
delay 120
screenshot /panel %APPDRIVE%/T2.BMP
gfx clear 1
gfx ellipse 180 90 12 12 0x00FFFF fill
gfx show
delay 120
screenshot /panel %APPDRIVE%/T3.BMP
gfx close
:done
echo [M-BTEAR-DONE]
"""

def _ball_centroid(bmp, rgb, tol=40, step=2):
    """Sampled centroid (x, y, count) of pixels near @p rgb."""
    sx = sy = n = 0
    for y in range(0, bmp.height, step):
        for x in range(0, bmp.width, step):
            r, g, b = bmp.px(x, y)
            if abs(r - rgb[0]) <= tol and abs(g - rgb[1]) <= tol and \
                    abs(b - rgb[2]) <= tol:
                sx += x
                sy += y
                n += 1
    if n == 0:
        return (0.0, 0.0, 0)
    return (sx / n, sy / n, n)


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
        if label.endswith("red"):
            # The canvas red arrives through several quantisations
            # (DOS cell red, RGB565 red); any strong red is the canvas.
            n = max(n, _count_red_family(bmp))
        c.check("%s pixels visible" % label, n >= minimum, "%d samples" % n)

    # CSS background-image + an inline PNG. Regression: LVGL's vendored
    # LodePNG returns an lv_draw_buf_t (not raw RGBA), so treating the pointer
    # as pixels turned every PNG white; a wrong channel order tinted a blue
    # background orange. The backdrop fixture is 16x16 #ff5555 (DOS red 12);
    # the inline fixture is a *different* tile (cyan/blue/white checker), so a
    # missing or white-blended inline image cannot hide inside the backdrop.
    _push(dev, "PIX.PNG", "WEB/PIX.PNG")
    _push(dev, "TILE.PNG", "WEB/TILE.PNG")
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
    # The inline image is a second, distinct PNG: its cyan/blue checks must be
    # on screen inside the framed box. A decode that returned nothing (the
    # inline box stayed empty) or the same image as the backdrop fails here -
    # the showcase used to render a solid red PNG on a red backdrop, which was
    # correct code but an invisible example.
    cyan = _count_near(bmp, (82, 255, 255))  # DOS bright cyan after cell quantisation
    c.check("inline PNG renders its own pixels", cyan >= 60,
            "%d cyan samples" % cyan)

    # ------------------------------------------------------------------
    # Present invariant: what the GLASS shows, not the LVGL model.
    # A model snapshot re-renders the object tree, so it cannot see a sprite
    # that was invalidated away or a stale pixel left on the panel. Twice this
    # release a gfx present was broken (trails, then the ball erased) while
    # every model screenshot looked correct. `screenshot /panel` reads the
    # MIPI-DSI frame buffer, and this fixture presents two colour-coded
    # frames: red must be on the panel in P1 and gone in P2, green the
    # reverse.
    # ------------------------------------------------------------------
    dev.run("draw close", timeout=20)
    dev.push_file("PRESENT.BAT", PRESENT_BAT.encode())
    s = dev.session
    s.reset_input()
    s.write_line("launch PRESENT.BAT")
    got = _read_until(s, "[M-PRESENT-DONE]", 60.0)
    c.check("present fixture completed", b"[M-PRESENT-DONE]" in got,
            got[-200:].decode("utf-8", "replace"))
    p1 = _pull_bmp(dev, c, "P1.BMP")
    p2 = _pull_bmp(dev, c, "P2.BMP")
    if p1 is not None and p2 is not None:
        red1 = _count_near(p1, (255, 0, 0), tol=40, step=2)
        green1 = _count_near(p1, (0, 255, 0), tol=40, step=2)
        red2 = _count_near(p2, (255, 0, 0), tol=40, step=2)
        green2 = _count_near(p2, (0, 255, 0), tol=40, step=2)
        c.check("present 1 reached the panel", red1 >= 100,
                "%d red panel samples" % red1)
        c.check("present 2 reached the panel", green2 >= 100,
                "%d green panel samples" % green2)
        c.check("present 2 replaced present 1 (no stale sprite)", red2 < 40,
                "%d stale red samples" % red2)
        c.check("present 1 had no stale green", green1 < 40,
                "%d stale green samples" % green1)
        moved = diff_ratio(p1, p2, tol=0)
        c.check("the two presents differ on the panel", moved >= 0.002,
                "%.5f of sampled pixels differ" % moved)
    dev.run("del /f P1.BMP & del /f P2.BMP", timeout=20)

    # ------------------------------------------------------------------
    # v1.4.4 bounce present: the direction change is where a single-framebuffer
    # DSI panel tears. `screenshot /panel` reads the framebuffer the panel
    # scans, so a stale old-position ball or a half-written new ball is visible
    # here even though an LVGL model snapshot would still look correct. Every
    # capture failure is a recorded check - no note()-only evidence.
    # ------------------------------------------------------------------
    dev.run("draw close", timeout=20)
    dev.push_file("BTEAR.BAT", BOUNCE_TEAR_BAT.encode())
    s.reset_input()
    s.write_line("launch BTEAR.BAT")
    got = _read_until(s, "[M-BTEAR-DONE]", 60.0)
    c.check("bounce present fixture completed", b"[M-BTEAR-DONE]" in got,
            got[-200:].decode("utf-8", "replace"))
    t1 = _pull_bmp(dev, c, "T1.BMP")
    t2 = _pull_bmp(dev, c, "T2.BMP")
    t3 = _pull_bmp(dev, c, "T3.BMP")
    if t1 is not None and t2 is not None and t3 is not None:
        RED = (255, 0, 0)
        GREEN = (0, 255, 0)
        CYAN = (0, 255, 255)
        nred1, ngreen1, ncyan1 = (_count_near(t1, k, tol=40, step=2)
                                  for k in (RED, GREEN, CYAN))
        nred2, ngreen2, ncyan2 = (_count_near(t2, k, tol=40, step=2)
                                  for k in (RED, GREEN, CYAN))
        nred3, ngreen3, ncyan3 = (_count_near(t3, k, tol=40, step=2)
                                  for k in (RED, GREEN, CYAN))
        rx1, _, _ = _ball_centroid(t1, RED)
        gx2, _, _ = _ball_centroid(t2, GREEN)
        cx3, _, _ = _ball_centroid(t3, CYAN)

        c.check("bounce frame 1 shows the red ball", nred1 >= 100,
                "%d red panel samples" % nred1)
        c.check("bounce frame 2 shows the green ball", ngreen2 >= 100,
                "%d green panel samples" % ngreen2)
        c.check("bounce frame 3 shows the cyan ball", ncyan3 >= 100,
                "%d cyan panel samples" % ncyan3)
        # A ball left behind at the old position, or a half-written ball, is a
        # tear at the direction change. Both colours must be absent outside
        # their own frame.
        c.check("bounce direction change left no stale red", nred2 < 40,
                "%d stale red samples in frame 2" % nred2)
        c.check("bounce reversal left no stale green", ngreen3 < 40,
                "%d stale green samples in frame 3" % ngreen3)
        c.check("bounce frame 1 had no other ball colours",
                ngreen1 < 40 and ncyan1 < 40,
                "green=%d cyan=%d" % (ngreen1, ncyan1))
        c.check("bounce frame 2 had no other ball colours",
                nred2 < 40 and ncyan2 < 40,
                "red=%d cyan=%d" % (nred2, ncyan2))
        c.check("bounce frame 3 had no other ball colours",
                nred3 < 40 and ngreen3 < 40,
                "red=%d green=%d" % (nred3, ngreen3))
        # The balls are fully drawn, not a partial (torn) stamp.
        c.check("bounce balls are fully drawn",
                400 <= nred1 <= 4000 and 400 <= ngreen2 <= 4000 and
                400 <= ncyan3 <= 4000,
                "red=%d green=%d cyan=%d" % (nred1, ngreen2, ncyan3))
        # Centroids prove a single ball moved right and then reversed, rather
        # than two half-balls split across the old and new positions.
        c.check("bounce ball advanced before the wall",
                gx2 - rx1 >= 0.20 * t2.width,
                "dx=%.1f px" % (gx2 - rx1))
        c.check("bounce ball reversed after the wall",
                gx2 - cx3 >= 0.05 * t3.width,
                "dx=%.1f px" % (gx2 - cx3))
    dev.run("del /f T1.BMP & del /f T2.BMP & del /f T3.BMP", timeout=20)

    try:
        os.makedirs(SPIKES, exist_ok=True)
        with open(os.path.join(SPIKES, "canvas_page.bmp"), "wb") as fh:
            fh.write(bmp.data)
        c.check("canvas screenshot captured", len(bmp.data) > 1000,
                "%d bytes" % len(bmp.data))
    except OSError as exc:  # noqa: BLE001 - a missing spike dir is a failure
        c.check("canvas screenshot captured", False, str(exc))

    return c
