# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Display suite: TUI / gfx / plot / font / theme / header / cursor.

Covers the three drawing surfaces side by side:

* **TUI** - the ``draw``/``tui``/``color``/``locate``/``anchor`` cell grid,
  screenshot-verified.
* **gfx** - the RGB565 canvas primitives plus sprite ``load``/``blit`` and a
  deterministic pixel check on the ``gfx save`` BMP (the same
  ``save`` + ``pull_file`` technique ``tools/gfx_toolkit_test.py`` uses).
* **plot** - the world-coordinate layer over both the canvas and the TUI,
  with a ``gfx save`` pixel palette check and a TUI screenshot.
* **font / theme / header / cursor** - state round-trips. Every state change
  is restored before returning so the next suite starts clean.
"""
import os
import re
import struct

from p4test.asserts import Checklist
from p4test.perf import parse_perf_report
from p4test.session import PanicError

NAME = "display"
TAGS = ["display", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

SPR = "_S08SPR.BMP"
GFX_BMP = "_S08GFX.BMP"
PLOT_BMP = "_S08PLOT.BMP"
LIST_TXT = "_S08LIST.TXT"
DATA_TXT = "_S08DATA.TXT"


# ---------------------------------------------------------------------------
# Host-side helpers
# ---------------------------------------------------------------------------
def q565(rgb):
    """RGB888 -> RGB565 -> RGB888 (the canvas quantization on save)."""
    r = (rgb >> 16) & 0xFF
    g = (rgb >> 8) & 0xFF
    b = rgb & 0xFF
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)


class _Bmp:
    """Minimal 24/32-bit BMP reader for a ``gfx save`` file (top-left origin)."""

    def __init__(self, raw):
        if len(raw) < 54 or raw[0:2] != b"BM":
            raise ValueError("not a BMP (%r)" % raw[:4])
        self.data = raw
        self.data_offset = struct.unpack_from("<I", raw, 10)[0]
        self.width = struct.unpack_from("<i", raw, 18)[0]
        self.height = struct.unpack_from("<i", raw, 22)[0]
        self.bpp = struct.unpack_from("<H", raw, 28)[0]
        self.top_down = self.height < 0
        self.height = abs(self.height)
        self.stride = ((self.width * self.bpp // 8 + 3) // 4) * 4

    def px(self, x, y):
        row = y if self.top_down else (self.height - 1 - y)
        off = self.data_offset + row * self.stride + x * (self.bpp // 8)
        b = self.data[off]
        g = self.data[off + 1]
        r = self.data[off + 2]
        return (r, g, b)

    def count_near(self, x, y, w, h, rgb, tol=24):
        tr, tg, tb = rgb
        n = 0
        for yy in range(max(0, y), min(self.height, y + h)):
            for xx in range(max(0, x), min(self.width, x + w)):
                r, g, b = self.px(xx, yy)
                if abs(r - tr) <= tol and abs(g - tg) <= tol and abs(b - tb) <= tol:
                    n += 1
        return n

    def region_mean(self, x, y, w, h):
        sr = sg = sb = 0
        n = 0
        for yy in range(max(0, y), min(self.height, y + h)):
            for xx in range(max(0, x), min(self.width, x + w)):
                r, g, b = self.px(xx, yy)
                sr += r
                sg += g
                sb += b
                n += 1
        if n == 0:
            return (0.0, 0.0, 0.0)
        return (sr / n, sg / n, sb / n)


def _make_bmp(w, h, rgb):
    """A 24-bit BI_RGB bottom-up BMP of a solid color (gfx sprite ingest)."""
    r, g, b = rgb
    row = w * 3
    stride = (row + 3) & ~3
    pix = bytearray()
    for _y in range(h):
        pix += bytes((b, g, r)) * w
        pix += b"\x00" * (stride - row)
    size = 54 + len(pix)
    header = (b"BM" + struct.pack("<IHHI", size, 0, 0, 54)
              + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pix),
                            2835, 2835, 0, 0))
    return header + bytes(pix)


# ---------------------------------------------------------------------------
# Suite
# ---------------------------------------------------------------------------
def run(dev, ctx):
    c = Checklist(NAME)
    disp_w, disp_h = dev.display_size()
    created = [GFX_BMP, PLOT_BMP]
    initial_theme = "default"
    initial_hdr = "auto"
    initial_cursor = "block"
    initial_blink = None

    def pull_bmp(remote):
        try:
            return _Bmp(dev.pull_file(remote))
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.note("pull %s failed: %s" % (remote, exc))
            return None

    def shot(name):
        try:
            return dev.screenshot(out_dir=OUT_DIR, name=name)
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.note("screenshot %s failed: %s" % (name, exc))
            return None

    try:
        # Push a small file for `draw list` and a two-column plot data file.
        dev.push_file(LIST_TXT, b"alpha\nbeta\ngamma\ndelta\n")
        created.append(LIST_TXT)
        dev.push_file(DATA_TXT, b"# x,y pairs\n0,0\n5,5\n-5,-5\n")
        created.append(DATA_TXT)

        # =================================================================
        # 1. TUI / draw
        # =================================================================
        c.note("TUI / draw verbs")
        try:
            dev.run("draw close", timeout=10)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass

        out = dev.run('draw box 2 2 34 10 double 7 0 "TUICAP" & echo S08_BOX=%ERRORLEVEL%')
        c.expect("draw box", "S08_BOX=0", out)
        out = dev.run('draw window 1 2 14 40 18 "WINTITLE" & echo S08_WIN=%ERRORLEVEL%')
        c.expect("draw window", "S08_WIN=0", out)
        dev.run("draw line 2 12 34 12 7 0")
        dev.run("draw fill 2 13 12 3 42 7 0")
        dev.run("draw text 3 17 TUI-TEXT 15 0")
        dev.run("draw bar 2 19 20 60 # - 10 0")
        dev.run('draw table 2 21 7 0 "H1|H2" "a|b" "c|d"')
        dev.run("draw list 2 25 20 6 %s" % LIST_TXT)
        out = dev.run("draw hold on & draw text 1 1 HOLD 7 0 & draw refresh & "
                      "draw hold off & echo S08_HOLD=%ERRORLEVEL%")
        c.expect("draw hold round-trip", "S08_HOLD=0", out)
        out = dev.run("draw cursor on & echo S08_CUR=%ERRORLEVEL%")
        c.expect("draw cursor on", "S08_CUR=0", out)
        dev.run("draw cursor off")
        out = dev.run("draw fullscreen on & echo S08_FS=%ERRORLEVEL%")
        c.expect("draw fullscreen on", "S08_FS=0", out)
        dev.run("draw fullscreen off")

        st = dev.run("tui status")
        c.expect("tui status active", "tui active", st)
        c.expect("tui status grid", "80 x 25", st)
        c.expect("tui status rect", "transcript rect", st)

        tui_raw = dev.run("tui stats")
        stats = parse_perf_report(tui_raw)
        c.check("tui stats parses", stats is not None, "raw=%r" % tui_raw[-200:])
        if stats is not None:
            c.check("tui stats frames sane", stats.frames >= 0, "frames=%d" % stats.frames)

        c.expect("color default", "color", dev.run("color"))
        c.expect("color set", "fg=7 bg=0", dev.run("color 7 0"))
        out = dev.run("locate 3 4 & echo S08_LOC=%ERRORLEVEL%")
        c.expect("locate", "S08_LOC=0", out)
        c.expect("anchor register", "anchor registered: S08ANCHOR",
                 dev.run("anchor S08ANCHOR echo hi"))

        tui_shot = shot("s08_tui")
        if tui_shot is not None:
            c.equals("TUI screenshot width", tui_shot.width, disp_w)
            c.equals("TUI screenshot height", tui_shot.height, disp_h)
            bright = tui_shot.count_near(0, 40, 1024, 480, (200, 204, 200), tol=32)
            c.check("TUI text visible on screen", bright > 0, "white px=%d" % bright)
        dev.run("draw close")

        # =================================================================
        # 2. GFX canvas + sprites
        # =================================================================
        c.note("gfx canvas primitives")
        try:
            dev.run("gfx close", timeout=10)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
        out = dev.run("gfx init 320 200")
        c.expect("gfx init", "canvas 320x200", out)
        c.expect("gfx status", "canvas 320x200", dev.run("gfx status"))

        # Solid 64x64 sprite for load/blit.
        dev.push_file(SPR, _make_bmp(64, 64, (255, 0, 255)))
        created.append(SPR)

        ops = [
            "gfx clear 0x101820",
            "gfx pixel 5 5 0xFFFFFF",
            "gfx hline 10 190 300 0xFFFFFF",
            "gfx vline 158 10 180 0xFFFF00",
            "gfx rect 20 20 40 30 0x00FF00 fill",
            "gfx rect 80 20 40 30 0x0000FF",
            "gfx circle 150 40 15 0x00FFFF fill",
            "gfx triangle 200 20 230 70 260 20 0xFF5555 fill",
            "gfx ellipse 60 90 20 30 0x55FF55 fill",
            "gfx polygon 0x5599FF fill 100 60 130 90 70 90",
            "gfx rect 240 10 40 40 0x808080",
            "gfx fill 260 30 0xFFAA00",
            "gfx text /scale:2 18 150 0xFFFFFF HELLO",
            "gfx text /scale:1 18 170 0x00FFFF world",
            "gfx load 0 %s" % SPR,
            "gfx blit 0 230 120",
            "gfx show",
        ]
        for op in ops:
            dev.run(op, timeout=15)

        c.expect("gfx slots lists sprite", "gfx.slot: 0 64x64", dev.run("gfx slots"))
        gstats = parse_perf_report(dev.run("gfx stats"))
        c.check("gfx stats parses", gstats is not None, "no frames field")
        if gstats is not None:
            c.check("gfx stats frames sane", gstats.frames >= 0,
                    "frames=%d" % gstats.frames)
        c.expect("gfx stats reset", "reset", dev.run("gfx stats reset"))
        c.expect("gfx stats target", "target 30 fps", dev.run("gfx stats target 30"))

        gfx_shot = shot("s08_gfx")
        if gfx_shot is not None:
            c.equals("gfx screenshot width", gfx_shot.width, disp_w)
            c.equals("gfx screenshot height", gfx_shot.height, disp_h)
            magenta = gfx_shot.count_near(0, 40, 1024, 480, (248, 0, 248), tol=40)
            c.check("gfx canvas sprite visible", magenta > 0, "magenta px=%d" % magenta)

        c.expect("gfx save", "gfx: saved", dev.run("gfx save %s" % GFX_BMP, timeout=30))
        dev.run("gfx free 0")
        c.check("gfx free clears slot", "gfx.slot:" not in dev.run("gfx slots"))
        dev.run("gfx close")

        b = pull_bmp(GFX_BMP)
        if b is not None:
            c.equals("gfx bmp dims", (b.width, b.height), (320, 200))
            c.equals("gfx pixel", b.px(5, 5), q565(0xFFFFFF))
            c.equals("gfx hline", b.px(100, 190), q565(0xFFFFFF))
            c.equals("gfx vline", b.px(158, 100), q565(0xFFFF00))
            c.equals("gfx rect fill", b.px(40, 35), q565(0x00FF00))
            c.equals("gfx rect outline", b.px(80, 35), q565(0x0000FF))
            c.equals("gfx rect interior", b.px(100, 35), q565(0x101820))
            c.equals("gfx circle fill", b.px(150, 40), q565(0x00FFFF))
            c.equals("gfx triangle fill", b.px(230, 45), q565(0xFF5555))
            c.equals("gfx ellipse fill", b.px(60, 90), q565(0x55FF55))
            c.equals("gfx polygon fill", b.px(100, 80), q565(0x5599FF))
            c.equals("gfx flood fill", b.px(260, 30), q565(0xFFAA00))
            c.equals("gfx sprite blit", b.px(260, 150), q565(0xFF00FF))
            c.check("gfx text scale2 white",
                    b.count_near(18, 150, 100, 16, q565(0xFFFFFF)) > 0)
            c.check("gfx text scale1 cyan",
                    b.count_near(18, 170, 100, 10, q565(0x00FFFF)) > 0)
        else:
            c.check("gfx bmp pulled", False, "pull failed")

        # =================================================================
        # 3. Plot (canvas + TUI)
        # =================================================================
        c.note("plot coordinate layer")
        dev.run("gfx init 320 200")
        c.expect("plot status canvas", "target=canvas", dev.run("plot status"))
        c.expect("plot window", "plot: window x=[-10, 10]", dev.run("plot window -10 10 -10 10"))
        c.expect("plot table", "plot.table: 4 8",
                 dev.run('plot table "X*2" /from:0 /to:4 /step:2'))
        c.expect("plot auto fit", "plot: window x=[-10, 10] y=",
                 dev.run('plot auto func "sin(X)"'))
        c.expect("plot func errorlevel", "P8_EL=0",
                 dev.run('plot func "sin(X)" /samples:64 & echo P8_EL=%ERRORLEVEL%'))
        c.expect("plot axes errorlevel", "P8_AX=0",
                 dev.run("plot axes /grid & echo P8_AX=%ERRORLEVEL%"))
        # `plot auto` narrowed the y-window to the sine range; reopen it so the
        # later data/bar/point/line/polar/para draws land inside the view.
        c.expect("plot window reopen", "plot: window x=[-10, 10]",
                 dev.run("plot window -10 10 -10 10"))
        c.expect("plot point errorlevel", "P8_PT=0",
                 dev.run("plot point 0 0 15 & echo P8_PT=%ERRORLEVEL%"))
        c.expect("plot line errorlevel", "P8_LN=0",
                 dev.run("plot line -10 -10 10 10 15 & echo P8_LN=%ERRORLEVEL%"))
        c.expect("plot data errorlevel", "P8_DT=0",
                 dev.run("plot data %s 14 & echo P8_DT=%%ERRORLEVEL%%" % DATA_TXT))
        c.expect("plot bar errorlevel", "P8_BR=0",
                 dev.run("plot bar 2,8,4,6 10 & echo P8_BR=%ERRORLEVEL%"))
        c.expect("plot polar errorlevel", "P8_PO=0",
                 dev.run('plot polar "1" 11 & echo P8_PO=%ERRORLEVEL%'))
        c.expect("plot para errorlevel", "P8_PA=0",
                 dev.run('plot para "5*cos(T)" "5*sin(T)" 13 & echo P8_PA=%ERRORLEVEL%'))
        dev.run("gfx save %s" % PLOT_BMP, timeout=30)
        dev.run("gfx close")

        pb = pull_bmp(PLOT_BMP)
        if pb is not None:
            c.equals("plot bmp dims", (pb.width, pb.height), (320, 200))
            c.check("plot axes gray drawn",
                    pb.count_near(0, 0, 320, 200, q565(0x555555)) > 0)
            c.check("plot func white drawn",
                    pb.count_near(0, 0, 320, 200, q565(0xFFFFFF)) > 0)
            c.check("plot data yellow drawn",
                    pb.count_near(0, 0, 320, 200, q565(0xFFFF55)) > 0)
            c.check("plot bar green drawn",
                    pb.count_near(0, 0, 320, 200, q565(0x55FF55)) > 0)
            c.check("plot polar cyan drawn",
                    pb.count_near(0, 0, 320, 200, q565(0x55FFFF)) > 0)
            c.check("plot para magenta drawn",
                    pb.count_near(0, 0, 320, 200, q565(0xFF55FF)) > 0)
        else:
            c.check("plot bmp pulled", False, "pull failed")

        # TUI target: `plot tui on` draws into the cell grid.
        out = dev.run("plot tui on")
        c.expect("plot tui target", "target=tui", out)
        c.expect("plot status tui", "target=tui", dev.run("plot status"))
        dev.run("plot window 0 10 0 10")
        dev.run("plot axes")
        dev.run('plot func "X" 15')
        plot_shot = shot("s08_plot_tui")
        if plot_shot is not None:
            c.equals("plot TUI screenshot width", plot_shot.width, disp_w)
            c.equals("plot TUI screenshot height", plot_shot.height, disp_h)
            # A plot is a thin line on the (near-black) TUI background, so the
            # whole-region mean is low even when the plot is drawn. Require a
            # clearly non-blank region rather than a bright one.
            c.check("plot TUI content visible",
                    sum(plot_shot.region_mean(0, 40, 1024, 480)) > 8.0,
                    "mean=%r" % (plot_shot.region_mean(0, 40, 1024, 480),))
        dev.run("plot tui off")
        dev.run("draw close")

        # =================================================================
        # 4. Font / theme / header / cursor
        # =================================================================
        c.note("font / theme / header / cursor")

        # -- font --
        flist = dev.run("font list")
        c.expect("font list builtin", "unscii_16", flist)
        c.expect("font list roles", "roles: terminal=", flist)
        c.expect("font info", "font roles:", dev.run("font info"))
        c.expect("font coverage", "[have] ascii:", dev.run("font coverage"))
        tm = re.search(r"terminal=(\S+)@(\d+)", flist)
        um = re.search(r"\bui=(\S+)@(\d+)", flist)
        term_name = tm.group(1) if tm else "unscii_16"
        ui_name = um.group(1) if um else "montserrat_14"
        ui_px = int(um.group(2)) if um else None
        # Switch each role to a name that `font list` proves is available
        # (built-in or an SD TTF) so the call always has a valid target. The
        # terminal font may legitimately be refused when it cannot fit the
        # 80x25 TUI grid ("breaks 80x25 ..."), which is still correct firmware
        # behaviour, so accept either outcome.
        term_out = dev.run("font set terminal %s" % term_name)
        c.check("font set terminal",
                ("font: terminal ->" in term_out) or ("breaks 80x25" in term_out),
                term_out[-200:])
        c.expect("font set ui", "font: ui ->", dev.run("font set ui %s" % ui_name))
        size_out = dev.run("font size ui 14")
        c.check("font size ui handled",
                ("font: ui size ->" in size_out)
                or ("fixed-size bitmap" in size_out)
                or ("breaks 80x25" in size_out), size_out[-200:])
        # Restore the captured roles (session-only; no /save anywhere).
        dev.run("font set terminal %s" % term_name)
        dev.run("font set ui %s" % ui_name)
        if ui_px:
            dev.run("font size ui %d" % ui_px)

        # -- theme --
        tlist = dev.run("theme list")
        for name in ("default", "amber", "ice", "mono"):
            c.expect("theme list %s" % name, name, tlist)
        active = re.search(r"^\s*\*\s+(\S+)", tlist, re.M)
        initial_theme = active.group(1) if active else "default"
        tshow = dev.run("theme show")
        c.expect("theme show label", "theme: ", tshow)
        c.expect("theme show colors", "bg: screen=", tshow)
        c.expect("theme set amber", "-> amber", dev.run("theme set amber"))
        c.expect("theme amber active", "amber (active)", dev.run("theme show"))
        c.expect("theme restore", "-> %s" % initial_theme,
                 dev.run("theme set %s" % initial_theme))
        c.expect("theme reject unknown", "unknown theme",
                 dev.run("theme set s08-no-such-theme"))

        # -- header --
        hdr = dev.run("header")
        c.expect("header mode reported", "header.mode=", hdr)
        hm = re.search(r"header.mode=(\w+)", hdr)
        initial_hdr = hm.group(1) if hm else "auto"
        for mode in ("full", "compact", "auto"):
            c.expect("header mode %s" % mode, "header.mode=%s" % mode,
                     dev.run("header mode %s" % mode))
        c.expect("header mode restore", "header.mode=%s" % initial_hdr,
                 dev.run("header mode %s" % initial_hdr))
        c.expect("header status", "header.height=", dev.run("header status"))

        # -- cursor --
        cur = dev.run("cursor /b")
        c.expect("cursor /b state", "CURSOR=", cur)
        cbm = re.search(r"CURSOR=(\w+)", cur)
        initial_cursor = cbm.group(1) if cbm else "block"
        blink = re.search(r"CURSOR_BLINK=(\d+)", cur)
        initial_blink = blink.group(1) if blink else "500"
        c.expect("cursor bar", "cursor bar", dev.run("cursor bar"))
        c.expect("cursor block", "cursor block", dev.run("cursor block"))
        dev.run("cursor blink off")
        dev.run("cursor blink %s" % initial_blink)
        c.expect("cursor status", "cursor:", dev.run("cursor status"))

    finally:
        # Restore every surface and remove the files this suite created.
        restore_cmds = ["plot tui off", "draw fullscreen off", "draw hold off",
                        "draw close", "gfx close",
                        "theme set %s" % initial_theme,
                        "header mode %s" % initial_hdr,
                        "cursor %s" % initial_cursor]
        if initial_blink is not None:
            restore_cmds.append("cursor blink %s" % initial_blink)
        for cmd in restore_cmds:
            try:
                dev.run(cmd, timeout=10)
            except PanicError:
                raise
            except Exception:  # noqa: BLE001
                pass
        for path in created:
            try:
                dev.run("del %s" % path, timeout=10)
            except PanicError:
                raise
            except Exception:  # noqa: BLE001
                pass

    return c
