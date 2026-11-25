# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""v1.7 rich suite: gfx tile/particle/layer, web images, interactive apps.

The host gates only prove host-side code is stable. This suite pushes the real
reference apps plus three scratch scripts to the SD card and asserts the marks
and pixel state the FIRMWARE emits over serial. Every check below is a real
observation of a device command or a BMP pulled from the device; no host
rendering is substituted and no check is recorded merely as evidence.

Coverage:
  * gfx tilemaps / particles / layers / atlas / image: a scratch batch drives
    each new verb with an `if errorlevel` tripwire, then its own 320x200 BMP is
    pulled and its tile, particle, layer and image regions are measured;
  * web image sink + fallback: a scratch HTML page with a good image, a
    missing image and an empty canvas is opened in the one DOM engine; the
    device-side `webapp images` report is parsed field by field;
  * interactive BOUNCE / GFXTOOL: preserved markers plus the new state, key,
    hit and life transitions, parsed and range-checked, and SHOWCASE's
    additive gfx-tools capability line;
  * honest limits: operator-only keys are not fabricated - only the bounded
    headless default path is asserted. A genuine hardware prerequisite (no SD
    card, no display) is a loud failure, never a green wash.
"""
from __future__ import annotations

import os
import re
import sys
import tempfile
from pathlib import Path

from p4test.asserts import Checklist
from p4test.screenshot import parse_bmp
from p4test.session import PanicError

NAME = "gfx_web_rich"
TAGS = ["apps", "gfx", "webapp", "slow"]
# Earlier suites leave a TUI page / rotated grid / modal behind; the pixel
# reads in this suite must start from a clean frame.
NEEDS_CLEAN_BOOT = True

ROOT = Path(__file__).resolve().parents[2]
APPS = ROOT / "apps"
OUT_DIR = os.path.join("screenshots", "regression")
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
DOS_LINE_RE = re.compile(
    r'^\[M-DOS-[A-Z0-9_-]+\]\s+screen=(\S+)\s+panels=(\d+)\s+'
    r'head="([^"]*)"\s+theme=(amber|green|ice|mono)\s+bars=(\d+)\s+'
    r'table=([YN])\s+spark=(\d+)\s+status=([YN])', re.MULTILINE)

sys.path.insert(0, str(APPS))
from app_manifest import APP_PAYLOADS, payload_path, remote_target  # noqa: E402
from suites.s25_apps_gallery import _make_sprites  # noqa: E402 (one sprite recipe)

PAYLOAD_NAMES = {
    "APPLIB.BAT", "BOUNCE.BAT", "GFXTOOL.BAT", "SHOWCASE.BAT",
    "WEBAPP.BAT", "RICH.HTML",
}

# Scratch gfx probe. The /if errorlevel tripwires make a refused verb a loud
# marker; the batch never depends on wall-clock timing or previous app state.
S29_GFX = """@echo off
gfx init 320 200
if errorlevel 1 goto :nofx
echo [M-S29-GFX] start
gfx clear 0x101820
gfx load 0 BALL.BMP
gfx tile create 0 map0 0 4 4 16 16 /z:1
if errorlevel 1 echo [M-S29-GFX-ERR] tile-create
gfx tile set 0 1 1 0 /f:h
if errorlevel 1 echo [M-S29-GFX-ERR] tile-set
gfx tile draw 16 16 0
if errorlevel 1 echo [M-S29-GFX-ERR] tile-draw
gfx tile list
if errorlevel 1 echo [M-S29-GFX-ERR] tile-list
echo [M-S29-GFX-TILE] map0
gfx particles seed 4242
if errorlevel 1 echo [M-S29-GFX-ERR] particles-seed
gfx particles emit 8 120 90 24 0xFFFFFF
if errorlevel 1 echo [M-S29-GFX-ERR] particles-emit
gfx particles step 1
if errorlevel 1 echo [M-S29-GFX-ERR] particles-step
gfx particles draw 120 90
if errorlevel 1 echo [M-S29-GFX-ERR] particles-draw
gfx particles clear
if errorlevel 1 echo [M-S29-GFX-ERR] particles-clear
echo [M-S29-GFX-PARTICLE] 8
gfx layer bind 0 0 L0 200 20 128 /z:1
if errorlevel 1 echo [M-S29-GFX-ERR] layer-bind
gfx layer dirty 0 200 20 16 16
if errorlevel 1 echo [M-S29-GFX-ERR] layer-dirty
gfx layer visible 0 on
if errorlevel 1 echo [M-S29-GFX-ERR] layer-visible
gfx layer present all
if errorlevel 1 echo [M-S29-GFX-ERR] layer-present
echo [M-S29-GFX-LAYER] L0
gfx image PHOTO.BMP 232 154 80 32
if errorlevel 1 echo [M-S29-GFX-ERR] image
if not errorlevel 1 echo [M-S29-GFX-IMG] ok
gfx save S29GFX.BMP
if errorlevel 1 echo [M-S29-GFX-ERR] save
gfx close
echo [M-S29-GFX] done
exit /b 0
:nofx
echo [M-S29-GFX] no canvas
exit /b 1
"""

# Scratch web page: one good image, one genuinely missing image (alt must be
# shown), a background-image and an empty canvas (fallback content must show).
S29_HTML = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>P4 IMAGE PROBE</title>
<style>
body { color: #cccccc; background-color: #000000; }
div { background-image: url(S29PIX.BMP); }
</style></head><body>
<h1>[M-S29-WEB] image page</h1>
<img src="S29PIX.BMP" alt="P4 badge">
<img src="S29MISS.BMP" alt="NO SIGNAL">
<div id="bg">bg</div>
<canvas id="chart" width="64" height="8">CHART OFFLINE</canvas>
</body></html>
"""

BOUNCE_UI_RE = re.compile(
    r"\[M-BOUNCE-UI\] state=(\w+) score=(\d+) lives=(\d+) level=(\d+)")
BOUNCE_KEY_RE = re.compile(r"\[M-BOUNCE-KEY\] key=([a-z]) state=(\w+)")
BOUNCE_FRAME_RE = re.compile(r"\[M-BOUNCE-FRAME\] (\d+)")
BOUNCE_DONE_RE = re.compile(r"\[M-BOUNCE-DONE\] frames=(\d+)")
BOUNCE_STATES = {"start", "help", "play", "pause", "over", "quit"}
BOUNCE_KEYS = set("adpnqhrs")

GFX_UI_RE = re.compile(
    r"\[M-GFXTOOL-UI\] state=(\w+) prim=(\w+) param=(\d+) view=(\w+)")
GFX_KEY_RE = re.compile(r"\[M-GFXTOOL-KEY\] key=([+\-a-z]) prim=(\w+) param=(\d+)")
GFX_PRIMS = {"line", "rect", "circle", "ellipse", "polygon", "fill", "text",
             "gradient", "atlas", "composite"}
GFX_KEYS = set("lrcepftgam+-vsq")
GFX_VIEWS = {"before", "after"}

WEB_IMAGE_RE = re.compile(
    r"^webapp\.image kind=(img|background|canvas) src=(\S+) "
    r"state=(ok|resized|missing|fallback) native=(\d+x\d+) "
    r"fit=(\d+x\d+) alt=(.*)$", re.MULTILINE)


def _send(dev, c, command, timeout=60):
    """Run one console command; a transport failure is a loud check."""
    try:
        raw = dev.run(command, timeout=timeout)
        return ANSI_RE.sub("", raw.replace("\r\n", "\n").replace("\r", "\n"))
    except Exception as exc:  # noqa: BLE001 - device errors must not be silent
        c.check("console command %r" % command.split()[0], False,
                "%s: %s" % (type(exc).__name__, exc))
        return ""


def _deploy(dev, c, tmp):
    """Push only the payloads this suite drives, plus its own scratch files."""
    for subdir, name, target in APP_PAYLOADS:
        if name not in PAYLOAD_NAMES:
            continue
        try:
            dev.push_file(remote_target(name, target),
                          Path(payload_path(subdir, name)).read_bytes())
        except Exception as exc:  # noqa: BLE001
            c.check("push app payload %s" % name, False, str(exc))
    sprites = _make_sprites(tmp)
    photo = next(data for path, data in sprites if path.endswith("PHOTO.BMP"))
    for local, remote in sprites + [(None, "WEB/S29PIX.BMP")]:
        data = Path(local).read_bytes() if local else photo
        try:
            dev.push_file(remote, data)
        except Exception as exc:  # noqa: BLE001
            c.check("push sprite %s" % remote, False, str(exc))
    S29_WEB = ("@echo off\n"
               "webapp open /no sd:/WEB/RICH.HTML\n"
               "webapp event p4run click\n"
               "webapp tick\n"
               "webapp dump 1 40 /a\n"
               "echo [M-S29-WEB] event+tick dispatched\n")
    for name, text in (("S29GFX.BAT", S29_GFX), ("S29WEB.BAT", S29_WEB),
                       ("WEB/S29IMG.HTML", S29_HTML)):
        try:
            dev.push_file(name, text.encode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            c.check("push %s" % name, False, str(exc))


def _gfx_surface(dev, c):
    """The firmware really dispatches the new tile/particle/layer/image verbs."""
    usage = _send(dev, c, "gfx", timeout=30)
    for verb in ("tile", "particles", "layer", "image"):
        c.expect("gfx usage lists " + verb, verb, usage)
    c.expect("gfx usage keeps the atlas verb", "blitmany", usage)

    tiles = _send(dev, c, "gfx tile", timeout=30)
    for sub in ("create", "set", "draw", "free", "list"):
        c.expect("gfx tile usage lists " + sub, sub, tiles)
    parts = _send(dev, c, "gfx particles", timeout=30)
    for sub in ("seed", "emit", "step", "draw", "clear"):
        c.expect("gfx particles usage lists " + sub, sub, parts)
    layers = _send(dev, c, "gfx layer", timeout=30)
    for sub in ("bind", "dirty", "present", "visible"):
        c.expect("gfx layer usage lists " + sub, sub, layers)


def _count_ink(bmp, box, background, different=True):
    x0, y0, x1, y1 = box
    hits = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            px = bmp.px(x, y)
            same = max(abs(px[i] - background[i]) for i in range(3)) <= 4
            if same != different:
                hits += 1
    return hits


def _gfx_rich(dev, c):
    """Drive every new gfx verb, then measure the device's own saved BMP."""
    out = _send(dev, c, "S29GFX.BAT", timeout=120)
    c.expect("gfx rich probe started", "[M-S29-GFX] start", out)
    c.expect("gfx rich probe finished", "[M-S29-GFX] done", out)
    c.expect("gfx rich probe had a canvas", "[M-S29-GFX] no canvas", out,
             want=False)
    for marker in ("[M-S29-GFX-TILE] map0", "[M-S29-GFX-PARTICLE] 8",
                   "[M-S29-GFX-LAYER] L0", "[M-S29-GFX-IMG] ok"):
        c.expect("gfx rich emitted " + marker, marker, out)
    errors = re.findall(r"\[M-S29-GFX-ERR\] (\w+)", out)
    c.check("every new gfx verb was accepted", not errors, "errors=%r" % errors)

    try:
        bmp = parse_bmp(dev.pull_file("S29GFX.BMP"))
    except Exception as exc:  # noqa: BLE001
        c.check("gfx rich saved a decodable BMP", False,
                "%s: %s" % (type(exc).__name__, exc))
        return out
    c.equals("gfx rich canvas width", bmp.width, 320)
    c.equals("gfx rich canvas height", bmp.height, 200)
    bg = bmp.px(2, 2)
    tile = _count_ink(bmp, (16, 16, 80, 80), bg)
    particles = _count_ink(bmp, (90, 60, 150, 120), bg)
    layer = _count_ink(bmp, (200, 20, 232, 52), bg)
    image = _count_ink(bmp, (232, 154, 312, 186), bg)
    c.check("tilemap stamped sprite ink", tile > 5,
            "%d tile pixels, bg=%r" % (tile, bg))
    c.check("particle step+draw placed ink", particles > 0,
            "%d particle pixels" % particles)
    c.check("alpha layer placed ink on the canvas", layer > 3,
            "%d layer pixels" % layer)
    c.check("gfx image blit placed pixels", image > 10,
            "%d image pixels" % image)
    c.note("S29GFX.BMP is pulled from the device; no host proof is substituted")
    return out


def _web_images(dev, c):
    """The new image sink must report every case, and show alt/fallback text."""
    _send(dev, c, "webapp open /no sd:/WEB/S29IMG.HTML", timeout=60)
    report = _send(dev, c, "webapp images", timeout=60)
    raw = [line for line in report.splitlines()
           if line.startswith("webapp.image ")]
    parsed = WEB_IMAGE_RE.findall(report)
    c.check("webapp reports image outcomes", len(parsed) > 0,
            "tail=%r" % report[-260:])
    c.check("every webapp.image line is well formed",
            len(parsed) == len(raw), "%d raw, %d parsed" % (len(raw), len(parsed)))
    kinds = {row[0] for row in parsed}
    for kind in ("img", "background", "canvas"):
        c.check("webapp.image reports kind " + kind, kind in kinds,
                "kinds=%r" % sorted(kinds))

    good = [row for row in parsed if row[0] == "img" and row[1] == "S29PIX.BMP"]
    c.check("good image was decoded and drawn",
            len(good) == 1 and good[0][2] in ("ok", "resized"),
            "rows=%r" % good)
    if good:
        c.equals("good image alt text", good[0][5], "P4 badge")
        c.check("good image has a real native size",
                all(int(v) > 0 for v in good[0][3].split("x")),
                "native=%s" % good[0][3])
        c.check("good image has a fitted size",
                all(int(v) > 0 for v in good[0][4].split("x")),
                "fit=%s" % good[0][4])
    missing = [row for row in parsed if row[1] == "S29MISS.BMP"]
    c.check("missing image reports an honest state",
            len(missing) == 1 and missing[0][2] == "missing",
            "rows=%r" % missing)
    if missing:
        c.equals("missing image shows its alt text", missing[0][5],
                 "NO SIGNAL")
    canvas = [row for row in parsed if row[0] == "canvas"]
    c.check("empty canvas reports its fallback",
            len(canvas) == 1 and canvas[0][2] == "fallback",
            "rows=%r" % canvas)
    if canvas:
        c.equals("canvas fallback content is shown", canvas[0][5],
                 "CHART OFFLINE")

    # The image work must not break the shipped page's event/tick path.
    page = _send(dev, c, "WEBAPP.BAT", timeout=120)
    c.expect("WEBAPP page still publishes its marker", "[M-RICH-WEBAPP]", page)
    c.expect("WEBAPP page still finishes", "[M-RICH-WEBAPP-DONE]", page)
    # Drive the same event/tick probe directly: the v1.6 scratch file is owned
    # by s28, so this suite carries its own copy instead of depending on it.
    tick = _send(dev, c, "S29WEB.BAT", timeout=120)
    c.expect("event/tick probe finished",
             "[M-S29-WEB] event+tick dispatched", tick)
    c.check("event/tick changed the live page",
            "clicked" in tick or "ready" in tick,
            "dump tail=%r" % tick[-260:])


def _bounce(dev, c):
    """Headless interactive path: preserved markers + new control states."""
    # Each bounded choice /T defaults after 1s of play, so 240 ticks can take
    # several minutes on an untouched board; never time out a real game.
    out = _send(dev, c, "BOUNCE.BAT", timeout=420)
    c.expect("BOUNCE started", "[M-BOUNCE]", out)
    c.expect("BOUNCE finished", "[M-BOUNCE-DONE] frames=", out)
    identity = DOS_LINE_RE.findall(out)
    c.check("BOUNCE publishes one headless DOS identity", len(identity) == 1,
            "%d line(s)" % len(identity))
    if identity:
        screen, panels, head, theme, bars, table, spark, status = identity[0]
        c.check("BOUNCE DOS id is its own",
                screen.upper().startswith("BOUNCE-"), "screen=%r" % screen)
        c.check("BOUNCE honestly reports a headless page",
                int(panels) == 0 and int(bars) == 0 and int(spark) == 0 and
                table == "N" and status == "N" and head.upper() == "NONE" and
                theme == "mono",
                "panels=%s head=%r theme=%s bars=%s table=%s spark=%s status=%s"
                % (panels, head, theme, bars, table, spark, status))
    c.expect("BOUNCE gfx/language tour still runs",
             "[M-RICH-BOUNCE] smooth gfx + language tour", out)
    c.expect("BOUNCE func result is exact",
             "[M-RICH-BOUNCE-F] speed=13", out)

    frames = [int(n) for n in BOUNCE_FRAME_RE.findall(out)]
    done = BOUNCE_DONE_RE.findall(out)
    c.check("BOUNCE emitted physics frames", len(frames) > 0,
            "%d frame markers" % len(frames))
    c.check("BOUNCE frame counter advances by one",
            frames == list(range(1, len(frames) + 1)),
            "first=%r last=%r count=%d" % (
                frames[:1], frames[-1:], len(frames)))
    c.check("BOUNCE done count equals the last frame",
            len(done) == 1 and bool(frames) and int(done[0]) == frames[-1],
            "done=%r last=%r" % (done, frames[-1:]))

    states = BOUNCE_UI_RE.findall(out)
    c.check("BOUNCE reports UI state transitions", len(states) >= 2,
            "%d transitions" % len(states))
    c.check("BOUNCE UI states are legal",
            all(row[0] in BOUNCE_STATES for row in states),
            "states=%r" % sorted({row[0] for row in states}))
    c.check("BOUNCE UI starts in start state",
            bool(states) and states[0][0] == "start",
            "first=%r" % (states[:1],))
    c.check("BOUNCE UI ends in a terminal state",
            bool(states) and states[-1][0] in ("over", "quit"),
            "last=%r" % (states[-1:],))
    c.check("BOUNCE UI score is non-negative",
            all(int(row[1]) >= 0 for row in states),
            "scores=%r" % sorted({row[1] for row in states}))
    c.check("BOUNCE lives stay in range",
            all(0 <= int(row[2]) <= 3 for row in states),
            "lives=%r" % sorted({row[2] for row in states}))
    c.check("BOUNCE level is non-negative",
            all(int(row[3]) >= 0 for row in states),
            "levels=%r" % sorted({row[3] for row in states}))

    keys = BOUNCE_KEY_RE.findall(out)
    c.check("BOUNCE accepted bounded default keys", len(keys) > 0,
            "%d key markers" % len(keys))
    c.check("BOUNCE key tokens are legal",
            all(row[0] in BOUNCE_KEYS for row in keys),
            "keys=%r" % sorted({row[0] for row in keys}))
    c.check("BOUNCE headless path used its default start/quit keys",
            bool({row[0] for row in keys} & {"s", "q"}),
            "keys=%r" % sorted({row[0] for row in keys}))

    try:
        bmp = parse_bmp(dev.pull_file("BOUNCE.BMP"))
    except Exception as exc:  # noqa: BLE001
        c.check("BOUNCE saved a decodable screenshot", False,
                "%s: %s" % (type(exc).__name__, exc))
        return out
    c.check("BOUNCE screenshot matches the live panel",
            (bmp.width, bmp.height) == dev.display_size(),
            "frame=%dx%d live=%r" % (bmp.width, bmp.height,
                                     dev.display_size()))
    return out


def _gfxtool(dev, c):
    """Preserved GFXTOOL markers plus the new selection/view control states."""
    out = _send(dev, c, "GFXTOOL.BAT", timeout=180)
    for marker in ("[M-GFXTOOL]", "[M-RICH-GFXTOOL]",
                   "[M-RICH-GFXTOOL-F] area=360", "[M-GFXTOOL-BLIT]",
                   "[M-RICH-GFXTOOL-ATLAS] sheet+composite stamped",
                   "[M-RICH-GFXTOOL-TILELAYER] tile map + alpha layer stamped",
                   "[M-GFXTOOL-DONE]"):
        c.expect("GFXTOOL emitted " + marker, marker, out)
    identity = DOS_LINE_RE.findall(out)
    c.check("GFXTOOL publishes one headless DOS identity", len(identity) == 1,
            "%d line(s)" % len(identity))
    if identity:
        c.check("GFXTOOL DOS id is its own",
                identity[0][0].upper().startswith("GFXTOOL-"),
                "screen=%r" % identity[0][0])
    img = "[M-GFXTOOL-IMG]" in out
    noimg = "[M-GFXTOOL-NOIMG]" in out
    c.check("GFXTOOL reports exactly one image outcome", img != noimg,
            "img=%s noimg=%s" % (img, noimg))

    states = GFX_UI_RE.findall(out)
    c.check("GFXTOOL reports UI state transitions", len(states) > 0,
            "%d transitions" % len(states))
    c.check("GFXTOOL UI states are legal",
            all(row[0] in ("menu", "done") for row in states),
            "states=%r" % sorted({row[0] for row in states}))
    c.check("GFXTOOL UI reaches done",
            any(row[0] == "done" for row in states),
            "states=%r" % sorted({row[0] for row in states}))
    c.check("GFXTOOL UI primitives are legal",
            all(row[1] in GFX_PRIMS for row in states),
            "prims=%r" % sorted({row[1] for row in states}))
    c.check("GFXTOOL UI params are non-negative",
            all(int(row[2]) >= 0 for row in states),
            "params=%r" % sorted({row[2] for row in states}))
    c.check("GFXTOOL UI views are legal",
            all(row[3] in GFX_VIEWS for row in states),
            "views=%r" % sorted({row[3] for row in states}))

    # The bounded default reads quit immediately, so a headless run publishes
    # no KEY marker: those are operator-driven. Assert the bound instead of
    # fabricating key presses.
    menus = sum(1 for row in states if row[0] == "menu")
    c.check("GFXTOOL workbench stopped within its bounded turns",
            menus >= 1 and menus <= 24, "menu states=%d" % menus)
    keys = GFX_KEY_RE.findall(out)
    c.check("any operator key that did arrive is legal",
            all(row[0] in GFX_KEYS for row in keys),
            "keys=%r" % sorted({row[0] for row in keys}))
    return out


def _showcase(dev, c):
    """The flagship app lists every rich gfx verb in one identity line."""
    out = _send(dev, c, "SHOWCASE.BAT", timeout=180)
    c.expect("SHOWCASE ran", "[M-RICH-SHOWCASE]", out)
    c.expect("SHOWCASE finished", "[M-SHOWCASE-DONE]", out)
    line = re.search(r"\[M-SHOWCASE-GFX-TOOLS\] ([^\r\n]+)", out)
    c.check("SHOWCASE publishes a gfx-tools line", line is not None,
            "tail=%r" % out[-240:])
    if line:
        verbs = set(line.group(1).split())
        for verb in ("lineaa", "rectaa", "circleaa", "gradient", "composite",
                     "blitmany", "image", "tile", "particles", "layer"):
            c.check("SHOWCASE lists gfx verb " + verb, verb in verbs,
                    "line=%r" % line.group(1))
    return out


def run(dev, ctx):
    c = Checklist(NAME)
    # The canvas is a shared display surface: no TUI page or stale gfx canvas
    # may survive into the pixel checks.
    _send(dev, c, "draw close", timeout=30)
    _send(dev, c, "tui fullscreen off", timeout=30)
    _send(dev, c, "gfx close", timeout=30)
    with tempfile.TemporaryDirectory(prefix="s29_gfx_web_") as tmp:
        _deploy(dev, c, tmp)
        _gfx_surface(dev, c)
        _gfx_rich(dev, c)
        _web_images(dev, c)
        _bounce(dev, c)
        _gfxtool(dev, c)
        _showcase(dev, c)
        for name in ("S29GFX.BAT", "S29GFX.BMP", "S29WEB.BAT",
                     "WEB/S29IMG.HTML", "WEB/S29PIX.BMP"):
            _send(dev, c, "del /f %s" % name, timeout=20)
    return c
