# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""v1.6 feature suite: every additive verb proves itself ON THE BOARD.

The host gates (``tools/host_all_gates.py``) only prove that host-side code is
stable. This suite closes the other half: it pushes the shipped reference apps
plus three tiny generated scripts to the SD card and asserts the v1.6 markers
and command outputs that the real firmware emits over serial.

Coverage (one or more real checks per row):
  * gfx lineaa / circleaa / rectaa / gradient / composite / blitmany /a:
    firmware usage line, GFXTOOL's documented markers, and a pulled 320x200
    BMP whose gradient and atlas stamp pixels are measured;
  * native info / status / verify + the natrun shim: information fields,
    usage text, and a deliberately corrupted payload that must be refused
    (never executed);
  * batch func/return %RESULT%, for /K, trap /on, str /rev|count|pad,
    map /values|incr: deterministic P4V16.BAT markers plus CONTROL/ELITE;
  * mpick / mprog / mformx / mstack: their real command surfaces, driven with
    short ``/t:`` timeouts so the suite cannot hang on a modal;
  * draw run|sprite and tui frame|focus|layout|scroll|route: on-device fields,
    dirty-rect transitions, and real geometry / clamping;
  * webapp event|tick: RICH.HTML's DOM state changes, read back with
    ``webapp dump`` from the one browser engine;
  * the DOS dashboard identity of the shipped apps: each real screen publishes
    its own ``[M-DOS-*]`` panels/header/theme/status line, and a live dashboard
    frame is checked through the ONE p4test pixel model for DOS palette ink and
    the absence of LVGL default-theme blue.

Honest limits: a genuine signature verification is NOT attempted with the
host fixture key (it is test-only and must never be installed on a board); a
corrupt envelope is used instead. No host rendering is mistaken for a device
capture.
"""
from __future__ import annotations

import os
import re
import sys
import tempfile
from pathlib import Path

from p4test.asserts import Checklist
from p4test import screenshot as shot
from p4test import theme_guard
from p4test.screenshot import parse_bmp
from p4test.session import PanicError

NAME = "v16_features"
TAGS = ["apps", "batch", "gfx", "tui", "modal", "webapp", "native", "slow"]
# Pixel/TUI state from earlier suites would contaminate the frame checks.
NEEDS_CLEAN_BOOT = True

ROOT = Path(__file__).resolve().parents[2]
APPS = ROOT / "apps"
OUT_DIR = os.path.join("screenshots", "regression")
# The one shipped DOS identity line every redesigned app publishes on the
# board and in its host fixture. Parsing the board text is a real assertion,
# not a host-side imitation of the app's screen.
DOS_LINE_RE = re.compile(
    r'^\[M-DOS-[A-Z0-9_-]+\]\s+screen=(\S+)\s+panels=(\d+)\s+'
    r'head="([^"]*)"\s+theme=(amber|green|ice|mono)\s+bars=(\d+)\s+'
    r'table=([YN])\s+spark=(\d+)\s+status=([YN])', re.MULTILINE)
# Only the apps whose shipped fixture is a framed dashboard are asserted as
# dashboards here; the gfx/native/control-flow apps honestly publish a
# headless identity and are asserted as such instead.
DOS_DASHBOARDS = (
    ("ELITE", "ELITE"), ("WEBAPP", "WEBAPP"), ("TASKMGR", "TMGR"),
)
DOS_HEADLESS = (
    ("CONTROL", "CONTROL"), ("GFXTOOL", "GFXTOOL"), ("HYBRID", "HYBRID"),
)
sys.path.insert(0, str(APPS))
from app_manifest import APP_PAYLOADS, payload_path, remote_target  # noqa: E402
from suites.s25_apps_gallery import _make_sprites  # noqa: E402 (one sprite recipe)

NATIVE_BAD = "V16BAD"
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
PAYLOAD_NAMES = {
    "APPLIB.BAT", "CONTROL.BAT", "ELITE.BAT", "GFXTOOL.BAT",
    "HYBRID.BAT", "WEBAPP.BAT", "INDEX.HTML", "RICH.HTML",
}

# Self-contained v1.6 language probe. Every marker is asserted below; nothing
# here depends on wall-clock time, random values or previous app state.
V16_BATCH = """@echo off
echo [M-S28-BATCH] start
func :s28double 21
echo [M-S28-FUNC] result=%RESULT%
map /set S28CFG host p4
map /set S28CFG port 80
map /values S28CFG S28V
map /incr S28CFG hits 2
for /K %%k in (S28CFG) do echo [M-S28-FORK] %%k=%%l
set S28S=p4mini
str /rev S28S S28R
str /count S28S i S28C
str /pad S28S 12 S28P -
echo [M-S28-STR] rev=%S28R% count=%S28C% pad=%S28P%
echo [M-S28-VALUES] 0=%S28V[0]% 1=%S28V[1]%
trap /on errorlevel :s28trap
trap /raise 4 "s28 probe"
echo [M-S28-TRAP] handler skipped (WRONG)
:s28trap
echo [M-S28-TRAP] code=%TRAP_CODE% msg=%TRAP_MSG%
trap /clear
echo [M-S28-BATCH] done
echo [M-HOST-BATCH-DONE]
exit /b 0
:s28double
return %1*2
"""

# The three modal verbs accept /t:SECS, so even an unattended board always
# returns. The markers record the published result variables (-1 on timeout is
# an honest value, not a fabricated success).
V16_MODAL = """@echo off
mpick /items Alpha^|Beta^|Gamma /title S28 /t:1 /v:V16PICK
echo [M-S28-MPICK] selected=%V16PICK%
mprog /run S28 /total 2 /step 1 /tick 50 /t:1 /v:V16PROG
echo [M-S28-MPROG] result=%V16PROG%
mformx /run S28 Name? /t:1 /v:V16FORM
echo [M-S28-MFORMX] result=%V16FORM%
mstack /push S28
mstack /top
mstack /list
mstack /pop
echo [M-S28-MSTACK] stack exercised
exit /b 0
"""

V16_WEB = """@echo off
webapp open /no sd:/WEB/RICH.HTML
if errorlevel 1 goto failed
webapp event p4run click
webapp tick
webapp dump 1 40 /a
echo [M-S28-WEB] event+tick dispatched
exit /b 0
:failed
echo [M-S28-WEB] page missing
exit /b 1
"""


def _send(dev, c, command, timeout=60):
    """Run one console command; a transport failure is a loud check."""
    try:
        raw = dev.run(command, timeout=timeout)
        return ANSI_RE.sub("", raw.replace("\r\n", "\n").replace("\r", "\n"))
    except Exception as exc:  # noqa: BLE001 - device errors must not be silent
        c.check("console command %r" % command.split()[0], False,
                "%s: %s" % (type(exc).__name__, exc))
        return ""


def _field(text, name):
    m = re.search(r"(?m)^\s*%s:\s*(.*)$" % re.escape(name), text)
    return m.group(1).strip() if m else None


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
    for local, remote in _make_sprites(tmp):
        try:
            dev.push_file(remote, Path(local).read_bytes())
        except Exception as exc:  # noqa: BLE001
            c.check("push sprite %s" % remote, False, str(exc))
    _send(dev, c, "mkdir NATIVE", timeout=20)
    bad = bytearray((ROOT / "tools" / "host" / "fixtures" / "native"
                     / "demo.P4X").read_bytes())
    bad[40] ^= 1  # CRC field: verifier must reject before any execution
    try:
        dev.push_file("NATIVE/%s.P4X" % NATIVE_BAD, bytes(bad))
    except Exception as exc:  # noqa: BLE001
        c.check("push corrupt native envelope", False, str(exc))
    for name, text in (("P4V16.BAT", V16_BATCH), ("P4V16M.BAT", V16_MODAL),
                       ("P4V16W.BAT", V16_WEB)):
        try:
            dev.push_file(name, text.encode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            c.check("push %s" % name, False, str(exc))


def _verb_surface(dev, c):
    """On-device usage/information probes: the verb really exists in firmware."""
    usage = _send(dev, c, "gfx", timeout=30)
    for verb in ("lineaa", "circleaa", "rectaa", "gradient", "composite",
                 "blitmany"):
        c.expect("gfx usage lists " + verb, verb, usage)
    c.expect("gfx blitmany atlas flag documented", "/a:<cols>,<cw>,<ch>", usage)

    info = _send(dev, c, "native info", timeout=30)
    for field in ("native: signed payload loader", "abi", "trusted key",
                  "verifier ready", "supported api", "max payload"):
        c.expect("native info reports " + field, field, info)
    native_usage = _send(dev, c, "native", timeout=30)
    c.expect("native usage names the read-only verb", "native info", native_usage)
    c.expect("native usage points at the single shim", "natrun", native_usage)
    shim = _send(dev, c, "natrun", timeout=30)
    c.expect("natrun shim is registered", "Usage: natrun <APP> [args]", shim)

    bad = _send(dev, c, "native verify %s" % NATIVE_BAD, timeout=45)
    c.expect("native verify reads the envelope", "native: %s:" % NATIVE_BAD, bad)
    c.expect("corrupt envelope never reported as verified", "verified", bad,
             want=False)
    c.note("a genuine signed verify needs the board's provisioned p4sign key; "
           "the test-only fixture key is never installed")

    web = _send(dev, c, "webapp", timeout=30)
    c.expect("webapp usage lists event", "webapp event", web)
    c.expect("webapp usage lists tick", "webapp tick", web)
    for verb in ("mpick", "mprog", "mformx", "mstack"):
        out = _send(dev, c, verb, timeout=30)
        c.expect("%s is registered on the board" % verb, "Usage: " + verb, out)


def _language(dev, c):
    """func/return, map, for /K, str and trap on the real batch interpreter."""
    out = _send(dev, c, "P4V16.BAT", timeout=120)
    c.expect("v1.6 batch probe started", "[M-S28-BATCH] start", out)
    c.expect("v1.6 batch probe finished", "[M-S28-BATCH] done", out)
    c.expect("func/return publishes RESULT", "[M-S28-FUNC] result=42", out)
    c.expect("map /values returns keys in slot order",
             "[M-S28-VALUES] 0=p4 1=80", out)
    keys = re.findall(r"\[M-S28-FORK\] (\w+)=(\S+)", out)
    c.check("for /K walked the map", len(keys) >= 3,
            "pairs=%r tail=%r" % (keys, out[-220:]))
    c.check("for /K binds the live incremented value",
            ("hits", "2") in keys, "pairs=%r" % (keys,))
    c.expect("str /rev|/count|/pad markers",
             "[M-S28-STR] rev=inim4p count=2 pad=p4mini------", out)
    c.expect("trap /on fires with published variables",
             "[M-S28-TRAP] code=4 msg=errorlevel 4", out)
    c.expect("trap handler was not skipped",
             "[M-S28-TRAP] handler skipped (WRONG)", out, want=False)

    # The shipped apps that document the same abilities must still emit them.
    control = _send(dev, c, "CONTROL.BAT", timeout=120)
    for marker in ("[M-RICH-CONTROL]", "[M-RICH-CONTROL-F]",
                   "[M-RICH-CONTROL-K]", "[M-RICH-CONTROL-S]",
                   "[M-RICH-CONTROL-T]", "[M-CONTROL-DONE]"):
        c.expect("CONTROL emitted " + marker, marker, control)
    elite = _send(dev, c, "ELITE.BAT", timeout=120)
    c.expect("ELITE market tour marker", "[M-RICH-ELITE]", elite)
    c.expect("ELITE map /values marker", "[M-RICH-ELITE-S] vals=", elite)
    c.expect("ELITE func result marker", "[M-RICH-ELITE-F] cargo=", elite)
    return {"CONTROL": control, "ELITE": elite}


def _gfx(dev, c, tmp):
    """Run the shipped toolkit app, then measure its own saved BMP."""
    out = _send(dev, c, "GFXTOOL.BAT", timeout=180)
    for marker in ("[M-GFXTOOL]", "[M-RICH-GFXTOOL]", "[M-RICH-GFXTOOL-F]",
                   "[M-RICH-GFXTOOL-ATLAS]", "[M-GFXTOOL-DONE]"):
        c.expect("GFXTOOL emitted " + marker, marker, out)
    try:
        bmp = parse_bmp(dev.pull_file("GFXTOOL.BMP"))
    except Exception as exc:  # noqa: BLE001
        c.check("GFXTOOL saved a decodable BMP", False,
                "%s: %s" % (type(exc).__name__, exc))
        return out
    c.equals("GFXTOOL canvas width", bmp.width, 320)
    c.equals("GFXTOOL canvas height", bmp.height, 200)
    left, right = bmp.px(40, 5), bmp.px(280, 5)
    c.check("gradient changes colour across the canvas", left != right,
            "left=%r right=%r" % (left, right))
    colours = {bmp.px(x, y) for y in range(4, 196, 8)
               for x in range(4, 316, 8)}
    c.check("canvas carries many raster colours", len(colours) > 10,
            "%d distinct sampled colours" % len(colours))
    # blitmany /a stamps the 16x16 BALL cells at (24,40), (40,40), (56,40).
    ink = sum(1 for y in range(42, 55)
              for x in range(28, 71)
              if min(bmp.px(x, y)) >= 200)
    c.check("atlas stamps put sprite ink on the canvas", ink > 5,
            "%d bright atlas pixels" % ink)
    c.note("GFXTOOL.BMP is pulled from the device; no host proof is substituted")
    return out


def _shipped_palette():
    """Read the one DOS palette from the shipped config header, not a copy."""
    text = (ROOT / "p4minishell_config.h").read_text(encoding="utf-8")
    names = ("BLACK", "RED", "GREEN", "YELLOW", "BLUE", "MAGENTA", "CYAN",
             "WHITE", "BRIGHT_BLACK", "BRIGHT_RED", "BRIGHT_GREEN",
             "BRIGHT_YELLOW", "BRIGHT_BLUE", "BRIGHT_MAGENTA",
             "BRIGHT_CYAN", "BRIGHT_WHITE")
    found = {}
    for name, value in re.findall(
            r"#define\s+P4_CONFIG_ANSI_([A-Z0-9_]+)\s+0x([0-9A-Fa-f]{6})", text):
        found[name] = tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))
    missing = [n for n in names if n not in found]
    if missing:
        raise RuntimeError("config header lacks DOS palette entries: %s"
                           % ", ".join(missing))
    return [found[n] for n in names]


def _dos_frame(dev, c):
    """Measure the LIVE dashboard frame with the one p4test pixel model.

    unwind=False is deliberate: TASKMGR owns the serial input at this point,
    and the screenshot retry's Ctrl-C would abort the app under test.
    """
    try:
        bmp = dev.screenshot(out_dir=OUT_DIR, name="s28_dos_taskmgr",
                             unwind=False)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001 - a miss is a real failed check
        c.check("live DOS dashboard frame captured", False,
                "%s: %s" % (type(exc).__name__, exc))
        return
    c.check("live DOS dashboard frame captured", bmp.width > 0 and bmp.height > 0,
            "decoded frame is %dx%d" % (bmp.width, bmp.height))
    c.equals("DOS frame matches the live panel", (bmp.width, bmp.height),
             dev.display_size())
    ink = sum(1 for y in range(0, bmp.height, 4)
              for x in range(0, bmp.width, 4)
              if sum(bmp.px(x, y)) > 240)
    c.check("DOS frame carries visible text ink", ink > 40,
            "%d bright sampled pixels" % ink)
    hits = theme_guard.scan_bmp(bmp)
    c.check("DOS frame has no LVGL default-theme blue", not hits,
            theme_guard.describe(hits))
    palette = _shipped_palette()
    samples = [bmp.px(x, y) for y in range(0, bmp.height, 4)
               for x in range(0, bmp.width, 4)]
    near = sum(1 for rgb in samples
               if any(max(abs(rgb[i] - p[i]) for i in range(3)) <= shot.TOL_LIVE
                      for p in palette))
    share = near / max(1, len(samples))
    c.check("DOS frame uses the shipped palette", share >= 0.50,
            "share=%.3f (%d/%d sampled)" % (share, near, len(samples)))


def _modal(dev, c):
    """Short-timeout modal surfaces plus real mstack session operations."""
    out = _send(dev, c, "P4V16M.BAT", timeout=120)
    c.expect("mpick ran and published a result", "[M-S28-MPICK] selected=", out)
    c.expect("mprog ran and published a result", "[M-S28-MPROG] result=", out)
    c.expect("mformx ran and published a result", "[M-S28-MFORMX] result=", out)
    c.expect("mstack session exercised", "[M-S28-MSTACK] stack exercised", out)
    top = _send(dev, c, "mstack /top", timeout=30)
    c.expect("mstack top is readable", "mstack/top:", top)
    lst = _send(dev, c, "mstack /list", timeout=30)
    c.expect("mstack list is readable", "mstack/list:", lst)
    interactive = _send(dev, c, "TASKMGR.BAT", timeout=20)
    c.expect("TASKMGR modal app starts", "[M-TMGR]", interactive)
    _dos_frame(dev, c)
    # Its full picker/progress path is operator-driven; stop before a modal
    # owns the worker so the following suites get a live shell.
    _send(dev, c, "\x03", timeout=20)
    return interactive


def _tui(dev, c):
    """draw run|sprite and the five tui surface sub-verbs, with real values."""
    _send(dev, c, "draw close", timeout=30)
    _send(dev, c, "draw clear screen", timeout=30)
    _send(dev, c, "tui frame reset", timeout=30)
    clean = _send(dev, c, "tui frame diff", timeout=30)
    c.expect("frame starts clean", "frame.dirty: no", clean)

    _send(dev, c, 'draw run 2 2 "[M-S28-RUN]" /fg 11 /bg 0', timeout=30)
    dirty = _send(dev, c, "tui frame diff", timeout=30)
    c.expect("draw run dirties the cell frame", "frame.dirty: yes", dirty)
    c.expect("dirty rect is reported", "frame.rect:", dirty)
    _send(dev, c, "tui frame flush", timeout=30)
    flushed = _send(dev, c, "tui frame diff", timeout=30)
    c.expect("one commit cleans the frame", "frame.dirty: no", flushed)

    _send(dev, c, "draw sprite 12 2 8 8 checker", timeout=30)
    sprite = _send(dev, c, "tui frame diff", timeout=30)
    c.expect("draw sprite dirties the cell frame", "frame.dirty: yes", sprite)
    _send(dev, c, "tui frame flush", timeout=30)

    status = _send(dev, c, "tui frame status", timeout=30)
    c.expect("frame is bound to the live grid", "frame: bound", status)
    grid = re.search(r"frame\.cols/rows:\s*(\d+)\s*x\s*(\d+)", status)
    c.check("frame reports a usable grid", grid is not None,
            "status=%r" % status[-180:])
    if grid is not None:
        cols, rows = int(grid.group(1)), int(grid.group(2))
        c.check("frame grid is at least a DOS page", cols >= 40 and rows >= 20,
                "grid=%dx%d" % (cols, rows))

    _send(dev, c, "ui /button S28BTN 1 1 8 OK /on S28OK", timeout=30)
    focus = _send(dev, c, "tui focus S28BTN", timeout=30)
    c.expect("tui focus selects a registered widget", "focus: S28BTN", focus)
    count = _field(focus, "focus.count")
    c.check("tui focus count is real", count is not None and int(count) >= 1,
            "focus.count=%r" % count)
    _send(dev, c, "ui /clear S28BTN", timeout=30)

    layout = _send(dev, c, "tui layout row 1 1 40 3 3", timeout=30)
    rects = re.findall(r"(?m)^\s*\d+\|(\d+),(\d+) (\d+)x(\d+)", layout)
    c.check("tui layout returned one rect per item", len(rects) == 3,
            "rects=%r tail=%r" % (rects, layout[-180:]))
    if len(rects) == 3:
        xs = [int(r[0]) for r in rects]
        c.check("row layout advances left to right", xs == sorted(xs) and
                len(set(xs)) == 3, "xs=%r" % (xs,))

    scroll = _send(dev, c, "tui scroll S28SC 4 20 3", timeout=30)
    c.expect("tui scroll applies a delta", "scroll.offset: 3", scroll)
    clamped = _send(dev, c, "tui scroll S28SC 4 20 99", timeout=30)
    c.expect("tui scroll clamps to content-viewport", "scroll.offset: 16",
             clamped)
    route = _send(dev, c, "tui route a", timeout=30)
    c.check("tui route reports a binding", _field(route, "route.label") is not None,
            "route=%r" % route[-180:])
    _send(dev, c, "draw close", timeout=30)
    _send(dev, c, "tui fullscreen off", timeout=30)


def _webapp(dev, c):
    """Drive the one DOM engine via webapp event/tick; assert DOM state."""
    app = _send(dev, c, "WEBAPP.BAT", timeout=120)
    c.expect("WEBAPP rich page marker", "[M-RICH-WEBAPP]", app)
    c.expect("WEBAPP event/tick done marker", "[M-RICH-WEBAPP-DONE]", app)
    out = _send(dev, c, "P4V16W.BAT", timeout=120)
    c.expect("webapp event/tick probe finished",
             "[M-S28-WEB] event+tick dispatched", out)
    c.expect("the old DOM state is gone", "boot", out, want=False)
    c.check("event/tick changed the live page",
            "clicked" in out or "ready" in out,
            "dump tail=%r" % out[-260:])
    return app


def _native_shim(dev, c):
    """The documented .bat front door must report an honest outcome."""
    out = _send(dev, c, "HYBRID.BAT", timeout=90)
    c.expect("HYBRID shim ran", "[M-HYBRID]", out)
    c.expect("HYBRID shim finished", "[M-HYBRID-DONE]", out)
    launched = "[M-RICH-HYBRID-NATIVE] signed P4X launched" in out
    skipped = "[M-RICH-HYBRID-SKIP] no signed demo payload installed" in out
    c.check("natrun shim reports launch or honest skip", launched or skipped,
            "tail=%r" % out[-240:])
    return out


def _dos_identity(c, outputs):
    """Assert each shipped app publishes its own DOS dashboard on the board.

    These are real lines emitted by the firmware apps; the fields are not
    inferred from host fixtures and no host rendering is substituted.
    """
    screens = {}
    for app, tag in DOS_DASHBOARDS:
        text = outputs.get(app, "")
        matches = DOS_LINE_RE.findall(text)
        c.check("%s publishes one DOS identity line" % app, len(matches) == 1,
                "%d line(s) in %d char(s) of output" % (len(matches), len(text)))
        if len(matches) != 1:
            continue
        screen, panels, head, theme, bars, table, spark, status = matches[0]
        c.check("%s DOS screen id is its own" % app,
                screen.upper().startswith(tag.upper() + "-"), "screen=%r" % screen)
        c.check("%s DOS dashboard has framed panels" % app, int(panels) >= 2,
                "panels=%s" % panels)
        c.check("%s DOS dashboard has a real header" % app,
                len(head) >= 3 and head.upper() != "NONE", "head=%r" % head)
        c.check("%s DOS dashboard uses a colour theme" % app,
                theme in ("amber", "green", "ice"), "theme=%r" % theme)
        c.check("%s DOS dashboard has panel content" % app,
                int(bars) > 0 or int(spark) > 0 or table == "Y" or status == "Y",
                "bars=%s table=%s spark=%s status=%s" % (bars, table, spark, status))
        screens[app] = (screen, head, panels, theme, bars, table, spark, status)
    c.check("shipped DOS dashboards have distinct signatures",
            len(set(screens.values())) == len(screens),
            "screens=%r" % {k: v[0] for k, v in sorted(screens.items())})
    c.check("all real DOS dashboards were exercised",
            len(screens) == len(DOS_DASHBOARDS),
            "%d/%d" % (len(screens), len(DOS_DASHBOARDS)))

    # Headless apps must not claim a dashboard they never draw.
    for app, tag in DOS_HEADLESS:
        text = outputs.get(app, "")
        matches = DOS_LINE_RE.findall(text)
        c.check("%s publishes one DOS identity line" % app, len(matches) == 1,
                "%d line(s)" % len(matches))
        if len(matches) != 1:
            continue
        screen, panels, head, theme, bars, table, spark, status = matches[0]
        c.check("%s DOS screen id is its own" % app,
                screen.upper().startswith(tag.upper() + "-"), "screen=%r" % screen)
        c.check("%s honestly reports a headless DOS page" % app,
                int(panels) == 0 and int(bars) == 0 and int(spark) == 0 and
                table == "N" and status == "N" and head.upper() == "NONE" and
                theme == "mono",
                "%s panels=%s head=%r theme=%s bars=%s table=%s spark=%s status=%s"
                % (screen, panels, head, theme, bars, table, spark, status))


def run(dev, ctx):
    c = Checklist(NAME)
    with tempfile.TemporaryDirectory(prefix="s28_v16_") as tmp:
        _deploy(dev, c, tmp)
        _verb_surface(dev, c)
        outputs: dict = {}
        outputs.update(_language(dev, c))
        outputs["GFXTOOL"] = _gfx(dev, c, tmp)
        outputs["TASKMGR"] = _modal(dev, c)
        _tui(dev, c)
        outputs["WEBAPP"] = _webapp(dev, c)
        outputs["HYBRID"] = _native_shim(dev, c)
        _dos_identity(c, outputs)
        for name in ("P4V16.BAT", "P4V16M.BAT", "P4V16W.BAT",
                     "NATIVE/%s.P4X" % NATIVE_BAD):
            _send(dev, c, "del /f %s" % name, timeout=20)
    return c
