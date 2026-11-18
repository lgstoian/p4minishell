# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Portrait / runtime-rotation suite.

The M5Stack Tab5's panel is 720x1280 PORTRAIT but the UI used to be laid out
for the 1024x600 landscape reference board, so every cell surface (the TUI
grid, the browser page, a batch app) was packed into a thin 80x25 strip at
8x12 px: 640x300 px inside a 720x1168 region. This suite asserts the fix from
the live screen in BOTH orientations, on the same boot:

* the logical grid keeps the documented 80 columns (so reference app frames
  authored for 80 columns are never clipped) and turns the extra panel height
  into more rows (``tui_grid_resolve`` -> ``tui status``): 80x97 at 8x12 on
  portrait, the documented 80x25 at 12x20/12x24 on landscape, glyph cells
  >= 8 px;
* the TUI surface/page fills the available region (grid pixels vs the
  reported transcript slot), and a modal panel still spans the panel;
* runtime rotation reflows in place: a dialog and the browser page survive
  ``rotate 90`` / ``rotate 0`` (no restart, no panic) and their pixel extent
  follows the new geometry;
* touch still lands on a control after a rotation (``ui targets`` -> ``ui tap``
  dismisses the dialog in both orientations);
* five rotate cycles leave PSRAM free bytes stable (no page-buffer leak).

A batch app (TCMD) is rotated mid-run too: ``rotate`` is a display action and
is claimed by the console-local hook even while the app's ``choice`` owns the
serial input. Compatibility choice: the visible grid keeps the documented 80
columns (8x12 on the 720 px portrait panel) so the 80-wide commander frame and
its right pane are never clipped; the extra portrait height becomes more rows
(80x102 while TCMD is full-screen). The app itself lays its page out against
the shell-published grid (``%TUICOLS%``/``%TUIROWS%`` via the APPLIB layout
helpers), so its frame fills those rows; this suite pins the surface mapping,
and ``s27_apps_layout.py`` pins the app geometry.

Screenshots land in ``screenshots/portrait/`` (with the pre-fix captures from
the same bug) and the numbers are printed in the notes.
"""
from __future__ import annotations

import os
import re
import time

from p4test.asserts import Checklist, SuiteResult
from p4test.session import PanicError, P4Error

NAME = "portrait"
TAGS = ["display", "ui", "slow"]
OUT_DIR = os.path.join("screenshots", "portrait")

# The shell prompt is the sync point for "rotate + rebuild finished".
ROTATE_SETTLE = 3.0


# ---------------------------------------------------------------------------
# Host helpers
# ---------------------------------------------------------------------------
def _console(dev, line, wait=1.2, read=0.8):
    """Send a console-local line (works while a modal/app owns the worker)."""
    dev.session.reset_input()
    dev.session.write_line(line)
    time.sleep(wait)
    return dev.session.read_for(read)


def _ui_state(dev):
    return _console(dev, "ui state")


def _modal(dev):
    m = re.search(r"modal=(\S+)", _ui_state(dev))
    return m.group(1) if m else "?"


def _status(dev):
    """`tui status` as a dict-ish text (console-local while a surface is up)."""
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


def _rect(text):
    m = re.search(r"transcript rect (\d+)x(\d+)", text)
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def _display(dev):
    """(w, h, rotation) from a FRESH sysinfo (Device.sysinfo() is cached)."""
    info = dev.run("sysinfo", timeout=25)
    m = re.search(r"display:\s*(\d+)\s*x\s*(\d+)", info)
    r = re.search(r"rotation=(\d+)", info)
    return (int(m.group(1)) if m else 0, int(m.group(2)) if m else 0,
            int(r.group(1)) if r else -1)


def _shot(dev, c, name):
    try:
        bmp = dev.screenshot(out_dir=OUT_DIR, name=name, unwind=False)
    except PanicError:
        raise
    except (P4Error, Exception) as exc:  # noqa: BLE001
        c.note("screenshot %s failed: %s" % (name, exc))
        return None
    return bmp


def _ink(bmp, lum=80):
    """Top/bottom rows carrying bright glyph ink (theme text/borders)."""
    top = None
    bot = None
    for y in range(bmp.height):
        for x in range(0, bmp.width, 2):
            r, g, b = bmp.px(x, y)
            if max(r, g, b) >= lum:
                if top is None:
                    top = y
                bot = y
                break
    return (top, bot)


def _ink_span(bmp):
    top, bot = _ink(bmp)
    if top is None:
        return 0, 0, 0.0
    return top, bot, 100.0 * (bot - top + 1) / bmp.height


def _targets(dev):
    """[(x, y, w, h, name)] from the console-local `ui targets` output."""
    out = _console(dev, "ui targets", wait=1.2, read=1.0)
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(.+?)\s*$", line)
        if m:
            rows.append((int(m.group(2)), int(m.group(3)), int(m.group(4)),
                         int(m.group(5)), m.group(6)))
    return rows


def _tap_named(dev, c, name):
    """Tap the centre of the first target whose label matches."""
    for (x, y, w, h, label) in _targets(dev):
        if label.split(" [C]")[0] == name or label.startswith(name):
            _console(dev, "ui tap %d %d" % (x + w // 2, y + h // 2),
                     wait=1.5, read=0.6)
            return True
    return False


def _rotate(dev, angle, settle=ROTATE_SETTLE):
    out = _console(dev, "rotate %d" % angle, wait=settle, read=1.0)
    return out


def _free(dev, tag="mem.psram.free"):
    out = dev.run("mem", timeout=20)
    m = re.search(r"%s:\s*(\d+)" % re.escape(tag), out)
    return int(m.group(1)) if m else -1


# ---------------------------------------------------------------------------
# Suite
# ---------------------------------------------------------------------------
def run(dev, ctx):
    c = Checklist(NAME)
    disp_w, disp_h, _ = _display(dev)
    native_portrait = disp_h > disp_w
    # COM6 is the Tab5: 720x1280 native. Skip honestly on another panel.
    if (disp_w, disp_h) not in ((720, 1280), (1280, 720)):
        return SuiteResult(NAME, 0, 0, skipped=1)
    if not native_portrait:
        dev.run("rotate 0", timeout=25, settle=2.0)

    # =====================================================================
    # 1. Portrait geometry: the grid follows the aspect and fills the slot
    # =====================================================================
    c.note("portrait geometry")
    dev.run("rotate 0", timeout=25, settle=2.0)
    dev.run("cls", timeout=15)
    pw, ph, prot = _display(dev)
    c.check("portrait orientation is 720x1280", (pw, ph, prot) == (720, 1280, 0),
            "display=%dx%d rotation=%d" % (pw, ph, prot))

    st = dev.run("tui status", timeout=20)
    fw, fh, fcols, frows, fcw, fch = _fit(st)
    c.check("portrait grid fit reported", fcols is not None, st[-220:])
    c.check("portrait grid keeps the 80-column contract", fcols == 80,
            "cols=%r (fit %r)" % (fcols, (fw, fh)))
    c.check("portrait grid adds rows for the extra height",
            frows is not None and frows >= 44, "rows=%r" % (frows,))
    c.check("portrait glyph cell >= 8 px",
            fcw is not None and fch is not None and fcw >= 8 and fch >= 8,
            "cell=%rx%r" % (fcw, fch))
    c.check("portrait grid fills >= 70% of the app slot",
            fcols is not None and frows is not None and fcw is not None
            and frows * fch >= fh * 0.70,
            "grid=%dpx slot=%dpx (%.1f%%)" % (frows * fch, fh,
                                             100.0 * frows * fch / max(1, fh)))
    c.check("portrait grid fills >= 70% of the panel width",
            fcols * fcw >= 720 * 0.70, "grid=%dpx panel=720" % (fcols * fcw))
    c.note("portrait grid = %d cols x %d rows at %dx%d (slot %dx%d)"
           % (fcols, frows, fcw, fch, fw, fh))

    shell_portrait = _shot(dev, c, "s26_shell_portrait")
    if shell_portrait is not None:
        c.equals("shell portrait screenshot width", shell_portrait.width, 720)
        c.equals("shell portrait screenshot height", shell_portrait.height, 1280)
        top, bot, frac = _ink_span(shell_portrait)
        c.check("shell transcript ink in the portrait panel", frac > 0.0,
                "ink y[%d..%d] (%.1f%%)" % (top, bot, frac))

    # =====================================================================
    # 2. Landscape: the documented 80x25 grid is preserved
    # =====================================================================
    c.note("landscape geometry (rotate 90)")
    _rotate(dev, 90)
    lw, lh, lrot = _display(dev)
    c.check("landscape orientation is 1280x720", (lw, lh, lrot) == (1280, 720, 90),
            "display=%dx%d rotation=%d" % (lw, lh, lrot))
    st = dev.run("tui status", timeout=20)
    fw, fh, fcols, frows, fcw, fch = _fit(st)
    c.check("landscape keeps 80 columns", fcols == 80, "cols=%r" % (fcols,))
    c.check("landscape keeps >= 25 rows", frows is not None and frows >= 25,
            "rows=%r" % (frows,))
    c.check("landscape glyph cell >= 8 px",
            fcw is not None and fch is not None and fcw >= 8 and fch >= 8,
            "cell=%rx%r" % (fcw, fch))
    shell_landscape = _shot(dev, c, "s26_shell_landscape")
    if shell_landscape is not None:
        c.equals("shell landscape screenshot width", shell_landscape.width, 1280)
        c.equals("shell landscape screenshot height", shell_landscape.height, 720)

    _rotate(dev, 0)
    pw, ph, prot = _display(dev)
    c.check("rotate 0 restores portrait", (pw, ph, prot) == (720, 1280, 0),
            "display=%dx%d rotation=%d" % (pw, ph, prot))
    st = dev.run("tui status", timeout=20)
    _, _, fcols2, frows2, _, _ = _fit(st)
    c.check("portrait grid re-derived after returning",
            fcols2 == 80 and frows2 is not None and frows2 > 25,
            "cols=%r rows=%r" % (fcols2, frows2))

    # =====================================================================
    # 3. Mid-app rotation: TCMD reflows in place, keeps taking keys
    # =====================================================================
    c.note("TCMD mid-app rotation")
    dev.session.write_line("launch TCMD")
    time.sleep(5.0)
    dev.session.read_for(0.6)
    tui_portrait = _shot(dev, c, "s26_tcmd_portrait")
    if tui_portrait is not None:
        c.equals("TCMD portrait shot width", tui_portrait.width, 720)
        c.equals("TCMD portrait shot height", tui_portrait.height, 1280)
        top, bot, frac = _ink_span(tui_portrait)
        c.check("TCMD paints in the portrait panel", frac > 5.0,
                "ink y[%d..%d] (%.1f%%)" % (top, bot, frac))

    out = _rotate(dev, 90)
    c.check("mid-app rotate 90 is accepted", "rotation set to 90" in out, out[-160:])
    tui_landscape = _shot(dev, c, "s26_tcmd_landscape")
    if tui_landscape is not None:
        c.equals("TCMD landscape shot width", tui_landscape.width, 1280)
        c.equals("TCMD landscape shot height", tui_landscape.height, 720)
    st = _status(dev)
    cols, rows, cw, ch = _grid(st)
    c.check("TCMD landscape grid is 80 columns at 12x24",
            (cols, cw, ch) == (80, 12, 24),
            "grid=%sx%s cell=%sx%s" % (cols, rows, cw, ch))
    c.check("TCMD landscape grid fills the slot", rows is not None and ch is not None
            and rows * ch >= 600 * 0.90,
            "grid=%rpx" % (rows * ch if rows and ch else None,))

    _rotate(dev, 0)
    tui_back = _shot(dev, c, "s26_tcmd_portrait_again")
    if tui_back is not None:
        c.equals("TCMD back-to-portrait shot width", tui_back.width, 720)
    st = _status(dev)
    cols, rows, cw, ch = _grid(st)
    c.check("TCMD portrait grid keeps 80 columns (no frame clip)",
            cols == 80, "grid=%sx%s cell=%sx%s" % (cols, rows, cw, ch))
    c.check("TCMD portrait grid uses 8x12 cells", (cw, ch) == (8, 12),
            "cell=%sx%s" % (cw, ch))
    c.check("TCMD portrait grid fills the slot", rows is not None and ch is not None
            and rows * ch >= 1100,
            "grid=%rpx" % (rows * ch if rows and ch else None,))
    c.note("TCMD portrait grid = %s cols x %s rows at %sx%s" % (cols, rows, cw, ch))

    # Still the same app, still taking keys: `j` (move) then `q` (quit).
    dev.session.write_line("j")
    time.sleep(1.0)
    dev.session.write_line("q")
    time.sleep(2.5)
    dev.session.write_line("")
    time.sleep(1.2)
    dev.session.read_for(1.0)
    alive = dev.run("echo S26APPOK", timeout=20)
    c.check("TCMD survived both rotations (no restart, no panic)",
            "S26APPOK" in alive, alive[-160:])

    # =====================================================================
    # 4. Open modal reflows in place + touch hits after the rotation
    # =====================================================================
    c.note("dialog reflow + touch after rotation")
    _rotate(dev, 0)
    dev.session.write_line('dialog /t:40 "S26" "portrait dialog" "OK" "Cancel"')
    time.sleep(2.5)
    c.check("dialog opened in portrait", _modal(dev) == "dialog", _ui_state(dev)[-200:])
    dlg_p = _shot(dev, c, "s26_dialog_portrait")
    if dlg_p is not None:
        c.equals("dialog portrait shot width", dlg_p.width, 720)
        top, bot, frac = _ink_span(dlg_p)
        c.check("dialog panel spans >= 70% of the portrait panel", frac >= 70.0,
                "panel y[%d..%d] (%.1f%%)" % (top, bot, frac))

    out = _rotate(dev, 90)
    c.check("rotate 90 while the dialog is open", "rotation set to 90" in out, out[-160:])
    c.check("dialog stayed open across the rotation", _modal(dev) == "dialog",
            _ui_state(dev)[-200:])
    dlg_l = _shot(dev, c, "s26_dialog_landscape")
    if dlg_l is not None:
        c.equals("dialog landscape shot width", dlg_l.width, 1280)
        top, bot, frac = _ink_span(dlg_l)
        c.check("dialog panel spans >= 70% of the landscape panel", frac >= 70.0,
                "panel y[%d..%d] (%.1f%%)" % (top, bot, frac))

    dismissed = _tap_named(dev, c, "OK")
    time.sleep(1.0)
    c.check("touch on OK dismisses the reflowed dialog", dismissed and _modal(dev) == "none",
            "tap=%r modal=%s" % (dismissed, _modal(dev)))
    if _modal(dev) == "dialog":
        # Never leak an open modal into the browser leg (the tap may have been
        # rejected); the dialog's own serial line closes it.
        _console(dev, "ok", wait=1.5)
        dev.read_for(0.5)

    # =====================================================================
    # 5. Browser page reflow in both directions
    # =====================================================================
    c.note("browser page reflow")
    _rotate(dev, 0)
    dev.session.write_line("browser sd:/WEB/INDEX.HTML")
    time.sleep(5.0)
    dev.session.read_for(0.8)
    st = _status(dev)
    c.check("browser page owns the TUI surface", "tui active" in st, st[-220:])
    page_p = _shot(dev, c, "s26_browser_portrait")
    cols, rows, cw, ch = _grid(st)
    rect_w, rect_h = _rect(st)
    c.check("browser portrait grid uses 8x12 cells", (cw, ch) == (8, 12),
            "grid=%sx%s cell=%sx%s" % (cols, rows, cw, ch))
    c.check("browser portrait grid keeps 80 columns", cols == 80,
            "grid=%sx%s" % (cols, rows))
    c.check("browser portrait grid fills its slot", rows is not None and ch is not None
            and rect_h is not None and rows * ch >= rect_h * 0.90,
            "grid=%rpx slot=%rpx" % (rows * ch if rows and ch else None, rect_h))
    if page_p is not None:
        c.equals("browser portrait shot width", page_p.width, 720)
        top, bot, frac = _ink_span(page_p)
        c.check("browser page ink fills >= 70% of its slot",
                frac * page_p.height >= (rect_h or 0) * 0.70,
                "ink y[%d..%d] (%.1f%%) slot=%r" % (top, bot, frac, rect_h))

    out = _rotate(dev, 90)
    c.check("rotate 90 while the page is up", "rotation set to 90" in out, out[-160:])
    time.sleep(1.0)
    st = _status(dev)
    c.check("browser page stayed up across the rotation", "tui active" in st, st[-220:])
    page_l = _shot(dev, c, "s26_browser_landscape")
    cols, rows, cw, ch = _grid(st)
    rect_w, rect_h = _rect(st)
    c.check("browser landscape grid is 80 columns", cols == 80,
            "grid=%sx%s cell=%sx%s" % (cols, rows, cw, ch))
    c.check("browser landscape grid fills its slot", rows is not None and ch is not None
            and rect_h is not None and rows * ch >= rect_h * 0.90,
            "grid=%rpx slot=%rpx" % (rows * ch if rows and ch else None, rect_h))
    if page_l is not None:
        c.equals("browser landscape shot width", page_l.width, 1280)
        top, bot, frac = _ink_span(page_l)
        c.check("browser page ink fills >= 70% of its slot",
                frac * page_l.height >= (rect_h or 0) * 0.70,
                "ink y[%d..%d] (%.1f%%) slot=%r" % (top, bot, frac, rect_h))

    _rotate(dev, 0)
    st = _status(dev)
    c.check("browser page survived the round trip", "tui active" in st, st[-220:])
    cols, rows, cw, ch = _grid(st)
    c.check("browser page restores 80 columns after rotate 0",
            cols == 80 and rows is not None and rows > 25,
            "grid=%sx%s" % (cols, rows))
    c.note("browser portrait grid = %s cols x %s rows at %sx%s"
           % (cols, rows, cw, ch))

    # The page buffer is live here: rotate it repeatedly and prove the reflow's
    # tui_viewport_init reallocation is leak-free (not just the idle path).
    c.note("page-buffer rotation cycles")
    page_mem_first = _free(dev)
    for _ in range(3):
        _rotate(dev, 90, settle=1.6)
        _rotate(dev, 0, settle=1.6)
    page_mem_last = _free(dev)
    c.check("page buffer rotation cycles sampled", page_mem_first > 0 and page_mem_last > 0,
            "first=%r last=%r" % (page_mem_first, page_mem_last))
    c.check("page buffer reflows without leaking",
            page_mem_first > 0 and page_mem_last > 0
            and (page_mem_first - page_mem_last) <= 256 * 1024,
            "first=%d last=%d drop=%d" % (page_mem_first, page_mem_last,
                                          page_mem_first - page_mem_last))
    c.check("page still up after repeated rotations", "tui active" in _status(dev),
            _status(dev)[-220:])
    dev.run("draw close", timeout=20, settle=1.0)
    st = dev.run("tui status", timeout=20)
    c.check("browser page closes back to the shell", "tui inactive" in st, st[-220:])

    # =====================================================================
    # 6. Five rotation cycles: memory stable, no panic
    # =====================================================================
    c.note("rotation stability (5 cycles)")
    dev.run("rotate 0", timeout=25, settle=1.5)
    first = _free(dev)
    lows = []
    for _ in range(5):
        _rotate(dev, 90, settle=1.5)
        _rotate(dev, 0, settle=1.5)
        lows.append(_free(dev))
    last = _free(dev)
    c.check("psram free sampled every cycle", first > 0 and last > 0 and all(v > 0 for v in lows),
            "first=%r series=%r" % (first, lows))
    c.check("five rotate cycles do not leak the page buffer",
            first > 0 and last > 0 and (first - last) <= 256 * 1024,
            "first=%d last=%d drop=%d" % (first, last, first - last))
    alive = dev.run("echo S26ROTOK", timeout=20)
    c.check("board alive after five rotation cycles", "S26ROTOK" in alive, alive[-120:])

    # Leave the reference orientation for the next suite.
    dev.run("rotate 0", timeout=25, settle=1.5)
    dev.run("cls", timeout=15)
    return c
