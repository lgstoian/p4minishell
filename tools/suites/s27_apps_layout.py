# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""App layout suite: the reference apps lay out against the LIVE grid.

The shell page already fills the portrait panel (the logical grid is 80x97 at
8x12 on a 720x1280 Tab5 instead of a 640x300 strip), but the batch apps used to
draw a hard-coded 80x25 page at the top of it: 22% of the panel, the rest
empty. This suite pins the fix in two halves:

Static (runs on any board, no app is launched)
  * every ``apps/**`` page that draws geometry reads the shell-published
    ``%TUICOLS%``/``%TUIROWS%`` grid or an ``APPLIB.BAT`` layout helper - an
    app that reintroduces a literal 80x25 frame fails here;
  * no ``draw box/list/fill/line/bar/image`` line carries a literal 80 or 25
    in a coordinate position (the classic grid is only allowed to appear as
    authored numbers *inside* a helper call, where it is resolved against the
    live grid);
  * ``APPLIB.BAT`` exposes the shared helpers and the apps use them (the
    one-layout-system rule, not 35 copies of width maths).

On the panel (Tab5 only)
  * portrait: TCMD's own BMP (its documented ``p`` snap key) spans the grid
    width and its frame reaches the bottom of the published grid - at least
    60% of the panel height, the acceptance bar for the bug;
  * portrait: the menu apps DIAG/SYS/NOTES/FILES paint a screen that fills at
    least 60% of the panel (their page frame plus the modal that owns the
    menu - modals are panel-height surfaces);
  * landscape: TCMD's frame is still the classic 25-row one (last ink row 23
    of the authored page) and still spans the 80-column grid, so the
    reference board did not move.

Screenshots land in ``screenshots/apps_layout/``. The numbers (grid, cell
font, panel size) come from ``tui status``/``sysinfo`` and the app's own BMP,
so the assertions are measured, not guessed.
"""
from __future__ import annotations

import glob
import os
import re
import time

from p4test.asserts import Checklist
from p4test.session import PanicError
from p4test.screenshot import parse_bmp

# Pixel/rotation state from the previous suite changes these captures.
NEEDS_CLEAN_BOOT = True

NAME = "apps_layout"
TAGS = ["apps", "display", "ui", "slow"]
OUT_DIR = os.path.join("screenshots", "apps_layout")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS_DIR = os.path.join(ROOT, "apps")
TCMD_BMP = "TCMD.BMP"

PORTRAIT = (720, 1280)
LANDSCAPE = (1280, 720)

# The acceptance bar from the bug report: an app's content block must cover at
# least this much of the portrait panel height.
MIN_PORTRAIT_COVER = 60.0

# Ordered apps the suite drives on the panel: TCMD is the frame witness (its
# ``p`` key writes its own BMP, no modal over the page), the rest are the menu
# pages the fix must also fill.
MENU_APPS = (
    dict(app="DIAG", start="[M-DIAG-MENU]", quit="9", end="[M-DIAG-DONE]"),
    dict(app="SYS", start="[M-SYS]", quit="6", end="[M-SYS-BACK]"),
    dict(app="NOTES", start="[M-NOTE]", quit="8", end=None, pre="ok"),
    dict(app="FILES", start="[M-FILES]", quit="9", end="[M-FILES-BACK]"),
)

# ``draw`` sub-verbs whose numeric arguments are grid coordinates.
GEOMETRY_VERB = re.compile(
    r"^\s*draw\s+(box|list|fill|line|bar|image|table|text)\b(.*)$", re.I)
# Any app page that paints the TUI (text included): these must derive from the
# published grid. `draw close/fullscreen/hold/refresh/clear` are lifecycle
# verbs, not page geometry.
DRAW_ANY = re.compile(r"^\s*draw\s+(box|list|fill|line|bar|image|table|text)\b",
                      re.I)
ADAPTS = ("TUICOLS", "TUIROWS", "APPLIB.BAT::")
LAYOUT_ROUTINES = (":box", ":page", ":bar", ":panel", ":footer")


# ---------------------------------------------------------------------------
# Host helpers (same idiom as s26_portrait / s25_apps_gallery)
# ---------------------------------------------------------------------------
def _console(dev, line, wait=1.2, read=0.8):
    """Send a console-local line (works while a modal/app owns the worker)."""
    dev.session.reset_input()
    dev.session.write_line(line)
    time.sleep(wait)
    return dev.session.read_for(read)


def _status(dev):
    return _console(dev, "tui status")


def _grid(text):
    """(cols, rows, cell_w, cell_h) from a `tui status` body, or Nones."""
    g = re.search(r"cols/rows (\d+) x (\d+)", text)
    c = re.search(r"cell font (\d+)x(\d+)", text)
    return (int(g.group(1)) if g else None,
            int(g.group(2)) if g else None,
            int(c.group(1)) if c else None,
            int(c.group(2)) if c else None)


def _fit(text):
    """(w, h, cols, rows, cell_w, cell_h) from the `grid fit` line."""
    m = re.search(r"grid fit (\d+)x(\d+) -> (\d+) x (\d+), cell (\d+)x(\d+)", text)
    if not m:
        return (None,) * 6
    return tuple(int(v) for v in m.groups())


def _display(dev):
    info = dev.run("sysinfo", timeout=25)
    m = re.search(r"display:\s*(\d+)\s*x\s*(\d+)", info)
    r = re.search(r"rotation=(\d+)", info)
    return (int(m.group(1)) if m else 0, int(m.group(2)) if m else 0,
            int(r.group(1)) if r else -1)


def _shot(dev, c, name):
    try:
        return dev.screenshot(out_dir=OUT_DIR, name=name, unwind=False)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("screenshot %s failed: %s" % (name, exc))
        return None


def _read_until(session, marker, timeout=30.0):
    try:
        return session.read_until(marker.encode(), timeout)
    except Exception:  # noqa: BLE001 - a missing marker is reported by the caller
        return b""


def _shell_echo(dev, tag="S27OK"):
    try:
        return tag in dev.run("echo " + tag, timeout=25)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        return False


def _restore_shell(dev, c, app):
    """Get back to a shell prompt so the next launch cannot eat its own line."""
    if _shell_echo(dev):
        return True
    for _ in range(3):
        try:
            dev.session.write(b"\x03\r\n")
        except Exception:  # noqa: BLE001
            pass
        dev.session.read_for(1.0)
        if _shell_echo(dev):
            c.note("%s: recovered the shell with a foreground break" % app)
            return True
    return False


# ---------------------------------------------------------------------------
# Pixel helpers
# ---------------------------------------------------------------------------
def _is_ink(bmp, x, y, lum=80):
    r, g, b = bmp.px(x, y)
    return max(r, g, b) >= lum


def _ink_box(bmp, lum=80):
    """(top, bottom, left, right) of the bright ink, or Nones when blank."""
    top = bottom = left = right = None
    for y in range(bmp.height):
        row_hit = False
        for x in range(0, bmp.width, 2):
            if _is_ink(bmp, x, y, lum):
                row_hit = True
                if left is None or x < left:
                    left = x
                if right is None or x > right:
                    right = x
        if row_hit:
            if top is None:
                top = y
            bottom = y
    return top, bottom, left, right


def _ink_row_band(bmp, y0, y1, lum=80):
    """(left, right) of the bright ink inside the row band [y0, y1)."""
    left = right = None
    for y in range(max(0, y0), min(bmp.height, y1)):
        for x in range(bmp.width):
            if _is_ink(bmp, x, y, lum):
                if left is None or x < left:
                    left = x
                if right is None or x > right:
                    right = x
    return left, right


def _ink_column_band(bmp, x0, x1, lum=80):
    """(top, bottom) of the bright ink inside the column band [x0, x1)."""
    top = bottom = None
    for x in range(max(0, x0), min(bmp.width, x1)):
        for y in range(bmp.height):
            if _is_ink(bmp, x, y, lum):
                if top is None or y < top:
                    top = y
                if bottom is None or y > bottom:
                    bottom = y
    return top, bottom


def _ink_fraction(bmp, lum=80):
    """Fraction of sampled pixels that carry ink (the not-blank gate)."""
    hits = 0
    total = 0
    for y in range(0, bmp.height, 4):
        for x in range(0, bmp.width, 4):
            total += 1
            if _is_ink(bmp, x, y, lum):
                hits += 1
    return (100.0 * hits / total) if total else 0.0


def _frame_rows(bmp, cell_w, cell_h):
    """Measure an app frame from its own capture.

    Returns a dict with the frame's ink rectangle in grid terms: the left/right
    pixels of its top border, its bottom-left ink row (1-based, relative to the
    frame's own first ink row) and the pixel height it covers.
    """
    top, bottom, _left, _right = _ink_box(bmp)
    if top is None or bottom is None:
        return None
    left, right = _ink_row_band(bmp, top, top + max(1, cell_h))
    if left is None:
        return None
    _ctop, cbottom = _ink_column_band(bmp, left, left + max(1, cell_w))
    bottom_row = ((cbottom - top) // cell_h + 1) if cbottom is not None else 0
    return dict(top=top, bottom=bottom, left=left, right=right,
                bottom_row=bottom_row, span_px=(right - left + 1),
                height_px=(bottom - top + 1))


# ---------------------------------------------------------------------------
# Static checks: the layout comes from the published grid, nowhere else
# ---------------------------------------------------------------------------
def _app_sources():
    return sorted(glob.glob(os.path.join(APPS_DIR, "**", "*.BAT"), recursive=True))


def _read_text(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _static_checks(c):
    literal = []
    unadapted = []
    drawing = []
    for path in _app_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if rel.endswith("APPLIB.BAT"):
            continue
        try:
            text = _read_text(path)
        except OSError as exc:  # noqa: BLE001
            c.check("read %s" % rel, False, str(exc))
            continue
        draws = False
        for number, line in enumerate(text.splitlines(), 1):
            if DRAW_ANY.match(line):
                draws = True
            m = GEOMETRY_VERB.match(line)
            if not m:
                continue
            before_title = m.group(2).split('"')[0]
            numbers = re.findall(r"(?<![\w%.])-?\d+(?![\w%])", before_title)
            if any(n in ("80", "25") for n in numbers):
                literal.append("%s:%d %s" % (rel, number, line.strip()))
        if draws:
            drawing.append(rel)
            if not any(token in text for token in ADAPTS):
                unadapted.append(rel)

    c.check("every reference app page that draws geometry was scanned",
            len(drawing) >= 15,
            "%d drawing BATs: %s" % (len(drawing), ", ".join(sorted(drawing))))
    c.check("no app hard-codes the 80x25 grid in draw geometry",
            not literal, "; ".join(literal))
    c.check("every drawing app reads the published grid or an APPLIB helper",
            not unadapted, "unadapted: %s" % ", ".join(unadapted))

    applib_path = os.path.join(APPS_DIR, "APPLIB.BAT")
    try:
        applib = _read_text(applib_path)
    except OSError as exc:  # noqa: BLE001
        c.check("APPLIB.BAT is readable", False, str(exc))
        return
    missing = [name for name in LAYOUT_ROUTINES
               if not re.search(r"(?m)^%s\s*$" % re.escape(name), applib)]
    c.check("APPLIB.BAT exposes the shared layout helpers", not missing,
            "missing %r" % missing)
    c.check("APPLIB layout helpers resolve the published grid",
            "TUICOLS" in applib and "TUIROWS" in applib,
            "APPLIB no longer reads %TUICOLS%/%TUIROWS%")
    users = 0
    for path in drawing:
        if "APPLIB.BAT::" in _read_text(os.path.join(ROOT, path)):
            users += 1
    c.check("the apps share the helpers instead of copying the maths",
            users >= 10, "%d drawing apps call APPLIB" % users)
    c.note("static layout scan: %d drawing BATs, 0 literal-grid coordinate(s)"
           % len(drawing))


# ---------------------------------------------------------------------------
# Panel drivers
# ---------------------------------------------------------------------------
def _tcmd_capture(dev, c, label):
    """Run TCMD to its main screen, take its own ``p`` snapshot, quit."""
    dev.run("del /f %s" % TCMD_BMP, timeout=20)
    s = dev.session
    s.reset_input()
    s.write_line("launch TCMD")
    head = _read_until(s, "[M-TCMD-R]", 60.0)
    if b"[M-TCMD-R]" not in head:
        c.note("%s: TCMD did not reach its page: %r" % (label, head[-200:]))
    time.sleep(2.0)
    st = _status(dev)
    cols, rows, cw, ch = _grid(st)
    # The shell cannot run a command while the app owns the worker, so the
    # capture has to be the app's own `p` snap. TCMD re-arms its key loop after
    # every refresh and a key sent while a refresh is in flight is dispatched as
    # a shell command (H7), so retry until the app itself acknowledges the snap
    # with its `ST=snapped` marker.
    snapped = False
    for _attempt in range(4):
        s.read_until(b"key?", 30.0)
        s.reset_input()
        s.write_line("p")
        got = _read_until(s, "ST=snapped", 15.0)
        if b"ST=snapped" in got:
            snapped = True
            break
        time.sleep(1.0)
    if not snapped:
        c.note("%s: snap key not acknowledged" % label)
    time.sleep(1.5)
    exited = False
    for _attempt in range(4):
        s.write_line("q")
        tail = _read_until(s, "[M-TCMD-DONE]", 12.0)
        if b"[M-TCMD-DONE]" in tail:
            exited = True
            break
        s.read_until(b"key?", 30.0)
    if not exited:
        c.note("%s: TCMD did not quit" % label)
    _restore_shell(dev, c, "TCMD/%s" % label)
    dev.run("delay 300", timeout=20)
    try:
        data = dev.pull_file(TCMD_BMP)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("%s: pull %s failed: %s" % (label, TCMD_BMP, exc))
        data = b""
    dev.run("del /f %s" % TCMD_BMP, timeout=20)
    bmp = None
    if data:
        try:
            os.makedirs(OUT_DIR, exist_ok=True)
            with open(os.path.join(OUT_DIR, "tcmd_%s.bmp" % label), "wb") as fh:
                fh.write(data)
        except Exception:  # noqa: BLE001
            pass
    if data:
        try:
            bmp = parse_bmp(data)
        except Exception as exc:  # noqa: BLE001
            c.note("%s: %s did not decode: %s" % (label, TCMD_BMP, exc))
    return bmp, (cols, rows, cw, ch)


def _drive_menu_app(dev, c, spec):
    """Launch one menu app, capture its screen, quit back to the shell."""
    app = spec["app"]
    s = dev.session
    s.reset_input()
    s.write_line("launch %s" % app)
    head = _read_until(s, spec["start"], 60.0)
    ok = spec["start"].encode() in head
    c.check("%s reaches its page" % app, ok, head[-200:])
    if spec.get("pre"):
        time.sleep(1.5)
        s.write_line(spec["pre"])
        time.sleep(1.5)
    time.sleep(2.0)
    bmp = _shot(dev, c, "s27_%s_portrait" % app.lower())
    s.read_for(0.5)
    s.write_line(spec["quit"])
    if spec.get("end"):
        tail = _read_until(s, spec["end"], 25.0)
        c.check("%s leaves its page cleanly" % app,
                spec["end"].encode() in tail, tail[-200:])
    else:
        time.sleep(2.0)
    c.check("%s returns to a live shell" % app, _restore_shell(dev, c, app), "")
    return bmp


# ---------------------------------------------------------------------------
# Suite
# ---------------------------------------------------------------------------
def run(dev, ctx):
    c = Checklist(NAME)

    # 1. Static: the layout is derived, not hard-coded (host-side, no panel).
    c.note("static layout audit")
    _static_checks(c)

    disp_w, disp_h, _ = _display(dev)
    native_portrait = disp_h > disp_w
    if (disp_w, disp_h) not in (PORTRAIT, LANDSCAPE):
        c.note("panel is %dx%d; the app-layout panel checks need the Tab5 "
               "(720x1280). Static audit above still ran." % (disp_w, disp_h))
        return c
    if not native_portrait:
        dev.run("rotate 0", timeout=25, settle=2.0)

    try:
        dev.run("draw close", timeout=15)
        dev.run("draw fullscreen off", timeout=15)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass

    # =====================================================================
    # 2. Portrait: the published grid, then TCMD's own 80x97 page
    # =====================================================================
    c.note("portrait app layout")
    dev.run("rotate 0", timeout=25, settle=2.0)
    st = dev.run("tui status", timeout=20)
    fw, fh, fcols, frows, fcw, fch = _fit(st)
    c.check("portrait publishes an app grid", fcols is not None and frows is not None,
            st[-220:])
    c.check("portrait app grid keeps the 80-column contract", fcols == 80,
            "cols=%r" % (fcols,))
    c.check("portrait app grid is taller than 25 rows",
            frows is not None and frows > 25, "rows=%r" % (frows,))

    bmp, live = _tcmd_capture(dev, c, "portrait")
    lcols, lrows, lcw, lch = live
    c.check("TCMD portrait grid reported live", lcols == 80 and lcw == 8 and lch == 12,
            "grid=%sx%s cell=%sx%s" % (lcols, lrows, lcw, lch))
    if bmp is None:
        c.check("TCMD portrait capture decoded", False, "no usable TCMD.BMP")
    else:
        c.equals("TCMD portrait capture width", bmp.width, 720)
        c.equals("TCMD portrait capture height", bmp.height, 1280)
        fr = _frame_rows(bmp, lcw or 8, lch or 12)
        c.check("TCMD portrait frame measured", fr is not None, "no ink in TCMD.BMP")
        if fr is not None:
            grid_px = (fcols or 80) * (fcw or 8)
            c.check("TCMD frame spans the logical grid width",
                    fr["span_px"] >= 0.95 * grid_px,
                    "frame=%dpx grid=%dpx" % (fr["span_px"], grid_px))
            c.check("TCMD portrait content covers >= 60%% of the panel",
                    fr["height_px"] >= MIN_PORTRAIT_COVER / 100.0 * 1280,
                    "frame=%dpx panel=1280 (%.1f%%)"
                    % (fr["height_px"], 100.0 * fr["height_px"] / 1280))
            c.check("TCMD portrait frame reaches the published grid bottom",
                    fr["bottom_row"] >= (frows or 97) - 3,
                    "last ink row=%d (grid rows=%r)"
                    % (fr["bottom_row"], frows))
            c.check("TCMD portrait frame is not clipped past the grid",
                    fr["bottom_row"] <= (lrows or frows or 97),
                    "last ink row=%d (live rows=%r)"
                    % (fr["bottom_row"], lrows))
            c.note("TCMD portrait frame: %dpx wide, %dpx tall, last ink row %d of %r"
                   % (fr["span_px"], fr["height_px"], fr["bottom_row"], frows))

    # =====================================================================
    # 3. Portrait: the menu pages fill the panel too
    # =====================================================================
    c.note("portrait menu pages")
    for spec in MENU_APPS:
        shot = _drive_menu_app(dev, c, spec)
        if shot is None:
            c.check("%s portrait capture decoded" % spec["app"], False,
                    "no streaming capture")
            continue
        c.equals("%s portrait capture width" % spec["app"], shot.width, 720)
        c.equals("%s portrait capture height" % spec["app"], shot.height, 1280)
        top, bottom, left, right = _ink_box(shot)
        ink = _ink_fraction(shot)
        c.check("%s portrait screen is not blank" % spec["app"],
                top is not None and ink >= 1.0,
                "ink=%.2f%%" % ink)
        if top is None:
            continue
        span = 100.0 * (bottom - top + 1) / shot.height
        c.check("%s portrait content covers >= 60%% of the panel" % spec["app"],
                span >= MIN_PORTRAIT_COVER,
                "ink y[%d..%d] (%.1f%% of 1280)" % (top, bottom, span))
        c.check("%s portrait content spans the grid width" % spec["app"],
                right - left + 1 >= 0.95 * (fcols or 80) * (fcw or 8),
                "ink x[%d..%d] = %dpx" % (left, right, right - left + 1))
        c.note("%s portrait: ink y[%d..%d] (%.1f%%), x[%d..%d]"
               % (spec["app"], top, bottom, span, left, right))

    # =====================================================================
    # 4. Landscape: the classic 80x25 frame is unchanged
    # =====================================================================
    c.note("landscape classic frame")
    out = _console(dev, "rotate 90", wait=3.0)
    c.check("rotate 90 lands in landscape", "rotation set to 90" in out, out[-160:])
    lw, lh, lrot = _display(dev)
    c.check("landscape orientation is 1280x720", (lw, lh, lrot) == (1280, 720, 90),
            "display=%dx%d rotation=%d" % (lw, lh, lrot))
    st = dev.run("tui status", timeout=20)
    _lw, _lh, lfcols, lfrows, lfcw, lfch = _fit(st)
    c.check("landscape publishes the classic app grid",
            lfcols == 80 and lfrows is not None and lfrows >= 25,
            "grid fit -> %r x %r (cell %rx%r)" % (lfcols, lfrows, lfcw, lfch))

    bmp, live = _tcmd_capture(dev, c, "landscape")
    lcols, lrows, lcw, lch = live
    c.check("TCMD landscape grid reported live",
            lcols == 80 and lcw == 12 and lch in (20, 24),
            "grid=%sx%s cell=%sx%s" % (lcols, lrows, lcw, lch))
    if bmp is None:
        c.check("TCMD landscape capture decoded", False, "no usable TCMD.BMP")
    else:
        c.equals("TCMD landscape capture width", bmp.width, 1280)
        c.equals("TCMD landscape capture height", bmp.height, 720)
        fr = _frame_rows(bmp, lcw or 12, lch or 24)
        c.check("TCMD landscape frame measured", fr is not None, "no ink in TCMD.BMP")
        if fr is not None:
            grid_px = 80 * (lfcw or 12)
            c.check("TCMD landscape frame spans the 80-column grid",
                    fr["span_px"] >= 0.95 * grid_px,
                    "frame=%dpx grid=%dpx" % (fr["span_px"], grid_px))
            # Today's commander inks its hint line on row 23 of the authored
            # 25-row page: that is what "landscape stays 80x25" means here.
            c.check("TCMD landscape keeps the classic 25-row frame",
                    22 <= fr["bottom_row"] <= 24,
                    "last ink row=%d (classic 23)" % fr["bottom_row"])
            c.note("TCMD landscape frame: %dpx wide, last ink row %d"
                   % (fr["span_px"], fr["bottom_row"]))

    # Leave the reference orientation for the next suite.
    dev.run("rotate 0", timeout=25, settle=1.5)
    dev.run("draw close", timeout=15)
    dev.run("draw fullscreen off", timeout=15)
    dev.run("cls", timeout=15)
    return c
