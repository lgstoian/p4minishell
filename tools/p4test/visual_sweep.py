# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""visual_sweep.py - capture every surface and run geometry/pixel invariants.

This is the visual-accuracy harness. It is test-side only (no firmware edits):

* navigates shell output, the header, the OSK, every modal, the editor, the
  TUI, the gfx canvas, the plot and the reference apps;
* captures a streaming BMPX screenshot of each (PNG + BMP on disk);
* parses `ui targets` / `ui state` / `header status` / `tui status`;
* runs structural invariants (on-screen, no overlap, row alignment, edge
  safety, header presence) and records violations as candidate findings;
* writes ``report.json``, a review draft and per-group contact sheets.

Usage:
    python tools/p4test/visual_sweep.py COM3 [--only GROUP,GROUP] [--no-apps]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from p4test import screenshot as p4shot               # noqa: E402
from p4test import visual                              # noqa: E402
from p4test.device import Device                       # noqa: E402
from p4test.session import P4Error, PanicError         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "screenshots", "visual")

# The transcript span-group container is a huge off-screen scroll object; it is
# not a UI control and must be excluded from overlap/bounds checks.
CONTAINER_NAMES = ("obj",)


@dataclass
class Finding:
    severity: str
    kind: str
    group: str
    name: str
    detail: str
    evidence: str = ""


@dataclass
class Capture:
    group: str
    name: str
    png: str
    bmp: str
    width: int = 0
    height: int = 0
    targets: List[visual.Target] = field(default_factory=list)
    notes: str = ""
    findings: List[Finding] = field(default_factory=list)


# --------------------------------------------------------------------------
# Invariant checks
# --------------------------------------------------------------------------

def check_targets(cap: Capture, screen_w: int, screen_h: int) -> None:
    """On-screen bounds, overlap and keyboard-grid alignment."""
    leaves = []
    for t in cap.targets:
        if t.name in CONTAINER_NAMES:
            continue
        # Clipped widgets (e.g. off-view scroll-list rows) are not visible or
        # tappable where reported; the firmware marks them for us.
        if t.clipped:
            continue
        # Ignore the known off-screen transcript scroll content.
        if t.y < 0 or t.y + t.h > screen_h + 40:
            continue
        leaves.append(t)

    for t in leaves:
        if t.x < 0 or t.x + t.w > screen_w or t.y < 0 or t.y + t.h > screen_h:
            cap.findings.append(Finding(
                "MEDIUM", "off-screen-widget", cap.group, cap.name,
                "target %r rect=(%d,%d,%d,%d) leaves the %dx%d screen"
                % (t.name, t.x, t.y, t.w, t.h, screen_w, screen_h),
                cap.png))

    for i in range(len(leaves)):
        for j in range(i + 1, len(leaves)):
            a, b = leaves[i], leaves[j]
            # A parent panel legitimately contains its child controls; only
            # sibling leaves that are neither nested nor obviously grouped are
            # a problem.
            if visual.within(a.rect, b.rect, 4) or visual.within(b.rect, a.rect, 4):
                continue
            if a.name.startswith("kbd:") != b.name.startswith("kbd:"):
                continue
            r = visual.overlap_ratio(a.rect, b.rect)
            if r > 0.15:
                cap.findings.append(Finding(
                    "MEDIUM", "widget-overlap", cap.group, cap.name,
                    "targets %r(%s) and %r(%s) overlap %.0f%%"
                    % (a.name, a.rect, b.name, b.rect, r * 100),
                    cap.png))

    kbd = [t for t in leaves if t.name.startswith("kbd:")]
    if len(kbd) >= 6:
        rows = visual.group_rows(kbd)
        for row in rows:
            row.sort(key=lambda t: t.x)
            for a, b in zip(row, row[1:]):
                if b.x < a.x + a.w - 2:
                    cap.findings.append(Finding(
                        "LOW", "kbd-key-overlap", cap.group, cap.name,
                        "keys %r and %r overlap on a row" % (a.name, b.name),
                        cap.png))
        heights = [max(t.h for t in row) for row in rows]
        if heights and (max(heights) - min(heights)) > 12:
            cap.findings.append(Finding(
                "LOW", "kbd-row-height", cap.group, cap.name,
                "keyboard row heights vary: %r" % heights, cap.png))


def check_pixels(cap: Capture, arr) -> None:
    """Header presence, background sanity and edge safety."""
    h, w = arr.shape[0], arr.shape[1]
    bg = visual.bg_color(arr)
    mask = visual.nonbg_mask(arr, bg, tol=24)

    # Fullscreen mode deliberately hides the header (windows_set_fullscreen ->
    # header_set_visible(false)), so the header band is expected to be empty.
    if cap.group != "boot" and "fullscreen" not in cap.name:
        ink = int(mask[:42, :].sum())
        if ink < 100:
            cap.findings.append(Finding(
                "MEDIUM", "header-empty", cap.group, cap.name,
                "top header band has almost no ink (ink=%d, bg=%r)" % (ink, bg),
                cap.png))

    # Ink touching the extreme LEFT/RIGHT screen edges usually means a
    # clipped/shifted element. The top edge is the header and the bottom edge
    # is the keyboard, so those are expected; full-bleed surfaces (TUI/gfx/apps)
    # legitimately reach every edge and are skipped.
    if cap.group not in ("tui", "gfx", "plot", "apps", "editor"):
        edges = visual.edge_ink(mask, margin=2)
        for side in ("left", "right"):
            count = edges.get(side, 0)
            if count > 0:
                cap.findings.append(Finding(
                    "LOW", "edge-ink", cap.group, cap.name,
                    "ink within 2px of the %s edge (count=%d)" % (side, count),
                    cap.png))


# --------------------------------------------------------------------------
# Screenshot helper
# --------------------------------------------------------------------------

class Sweep:
    def __init__(self, dev: Device, out_dir: str):
        self.dev = dev
        self.out = out_dir
        self.captures: List[Capture] = []
        self.findings: List[Finding] = []
        os.makedirs(out_dir, exist_ok=True)

    def _snap(self, group: str, name: str, notes: str = "",
              modal: bool = False, pulled_from: Optional[str] = None) -> Capture:
        gdir = os.path.join(self.out, group)
        os.makedirs(gdir, exist_ok=True)
        stem = os.path.join(gdir, name)
        bmp = None
        last_exc = None
        if pulled_from is not None:
            # Some apps own the serial input (TCMD's `choice` prompt), so an
            # injected streaming `screenshot` is consumed by the app. Pull the
            # BMP the app saved to SD instead (T1).
            try:
                raw = self.dev.pull_file(pulled_from)
                with open(stem + ".bmp", "wb") as fh:
                    fh.write(raw)
                bmp = p4shot._parse_bmp(raw)
                bmp.save_png(stem + ".png")
            except Exception as exc:  # noqa: BLE001
                last_exc = exc
                notes += " | pull %s failed: %s" % (pulled_from, exc)
        else:
            for attempt in range(3):
                try:
                    bmp = p4shot.capture(self.dev.session, out_dir=gdir, name=name)
                    break
                except Exception as exc:  # noqa: BLE001
                    last_exc = exc
                    notes += " | capture retry %d: %s" % (attempt + 1, exc)
                    # Drain any desynchronised stream and quiesce.
                    try:
                        self.dev.session.read_for(0.8)
                    except Exception:  # noqa: BLE001
                        pass
                    time.sleep(0.8)
        cap = Capture(group, name, stem + ".png", stem + ".bmp",
                      getattr(bmp, "width", 0), getattr(bmp, "height", 0),
                      notes=notes)
        if bmp is None:
            cap.findings.append(Finding(
                "MEDIUM", "capture-failed", group, name,
                "screenshot failed after 3 attempts: %s" % last_exc, cap.png))
            self.captures.append(cap)
            self.findings.extend(cap.findings)
            print("   [%-14s] %-28s CAPTURE-FAILED" % (group, name), flush=True)
            return cap
        if modal:
            # While a modal owns the worker, `ui targets` must be read raw: a
            # marker-synced run() queues an `echo <tag>` that the modal treats
            # as its answer and closes.
            try:
                session = self.dev.session
                session.reset_input()
                session.write_line("ui targets")
                cap.targets = visual.parse_targets(session.read_for(1.2))
            except Exception as exc:  # noqa: BLE001
                cap.notes += " | raw ui targets failed: %s" % exc
        else:
            try:
                cap.targets = visual.parse_targets(self.dev.run("ui targets", timeout=25))
            except Exception as exc:  # noqa: BLE001
                cap.notes += " | ui targets failed: %s" % exc
        arr = visual.array(bmp)
        check_pixels(cap, arr)
        check_targets(cap, bmp.width, bmp.height)
        self.captures.append(cap)
        self.findings.extend(cap.findings)
        tag = "OK" if not cap.findings else "FINDINGS=%d" % len(cap.findings)
        print("   [%-14s] %-28s %s" % (group, name, tag), flush=True)
        return cap

    def run_capture(self, group: str, name: str, cmd: Optional[str],
                    notes: str = "", timeout: float = 40.0) -> Capture:
        if cmd:
            try:
                self.dev.run(cmd, timeout=timeout)
            except Exception as exc:  # noqa: BLE001
                notes += " | %r failed: %s" % (cmd, exc)
        time.sleep(0.4)
        return self._snap(group, name, notes)

    def modal_capture(self, group: str, name: str, cmd: str, answer: str = "q",
                      settle: float = 2.0, notes: str = "") -> Capture:
        """Open a modal, wait until the runtime reports it (and the LVGL task
        has painted it), then capture. The panel is created asynchronously, so
        a fixed sleep races the first frame."""
        session = self.dev.session
        # Clear the scrollback first: modal panels open at the transcript
        # content origin, and a tall scrollback pushes them off-screen (that
        # bug is captured separately); clearing lets the panel layout be
        # reviewed.
        try:
            self.dev.run("cls", timeout=10)
        except Exception:  # noqa: BLE001
            pass
        session.reset_input()
        session.write_line(cmd)
        deadline = time.time() + 6.0
        opened = False
        while time.time() < deadline:
            session.reset_input()
            session.write_line("ui state")
            st = session.read_for(0.6)
            if "modal=" in st and "modal=none" not in st:
                opened = True
                break
        if not opened:
            notes += " | modal did not report open"
        time.sleep(settle)
        # Warm-up streaming capture: forces a full render of the new surface so
        # the next capture is not a stale pre-modal frame (discarded).
        try:
            p4shot.capture(self.dev.session, out_dir=None)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.7)
        cap = self._snap(group, name, notes, modal=True)
        # Only answer if the surface is still open (do not leak the answer as a
        # shell command).
        session.reset_input()
        session.write_line("ui state")
        st = session.read_for(0.7)
        if "modal=none" in st:
            cap.notes += " | modal already closed before answer"
        else:
            session.reset_input()
            session.write_line(answer)
            time.sleep(0.6)
            session.read_for(0.6)
        return cap


# --------------------------------------------------------------------------
# The sweep matrix
# --------------------------------------------------------------------------

def shell_surfaces(sw: Sweep) -> None:
    sw.run_capture("shell", "idle", "cls")
    sw.run_capture("shell", "prompt", "cd sd:/APPS")
    sw.run_capture("shell", "prompt_root", "cd sd:/")
    sw.run_capture("shell", "help", "help")
    sw.run_capture("shell", "help_all", "help /all", timeout=60)
    sw.run_capture("shell", "sysinfo", "sysinfo", timeout=60)
    sw.run_capture("shell", "about", "about")
    sw.run_capture("shell", "version", "version")
    sw.run_capture("shell", "mem", "mem")
    sw.run_capture("shell", "ps", "ps")
    sw.run_capture("shell", "tasks", "tasks")
    sw.run_capture("shell", "top", "top /b", timeout=40)
    sw.run_capture("shell", "debug", "debug")
    sw.run_capture("shell", "unknown_cmd", "p4-nonesuch-command")
    sw.run_capture("shell", "error_missing_file", "type P4NOPE.TXT")
    # A few listing/report commands whose layout is easy to get wrong.
    sw.run_capture("shell", "dir", "dir sd:/APPS")
    sw.run_capture("shell", "dir_wide", "dir sd:/APPS /w")
    sw.run_capture("shell", "dir_bare", "dir sd:/APPS /b")
    sw.run_capture("shell", "tree", "tree sd:/APPS /F", timeout=45)
    sw.run_capture("shell", "sd_info", "sd info", timeout=40)
    sw.run_capture("shell", "disk_detail", "disk detail", timeout=40)
    sw.run_capture("shell", "chkdsk", "chkdsk", timeout=90)
    sw.run_capture("shell", "config", "config")
    sw.run_capture("shell", "theme_list", "theme list")
    sw.run_capture("shell", "font_list", "font list")
    sw.run_capture("shell", "font_info", "font info")
    sw.run_capture("shell", "cursor_status", "cursor status")
    sw.run_capture("shell", "wifi_status", "wifi status", timeout=30)
    sw.run_capture("shell", "netstat", "netstat")
    sw.run_capture("shell", "ipconfig", "ipconfig")
    sw.run_capture("shell", "usb_status", "usb status")
    sw.run_capture("shell", "bt_status", "bluetooth status")
    sw.run_capture("shell", "power", "power")
    sw.run_capture("shell", "battery", "battery")
    sw.run_capture("shell", "gpio_list", "gpio list")
    sw.run_capture("shell", "adc_status", "adc status")
    sw.run_capture("shell", "i2c_scan", "i2c scan", timeout=40)
    sw.run_capture("shell", "rgb_status", "rgb status")
    sw.run_capture("shell", "launch_list", "launch /list")


def header_surfaces(sw: Sweep) -> None:
    sw.run_capture("header", "glyph", "header mode glyph")
    sw.run_capture("header", "words", "header mode words")
    sw.run_capture("header", "status_glyph", "header status")
    sw.run_capture("header", "hidden", "header hide")
    sw._snap("header", "hidden_shot")
    sw.run_capture("header", "shown", "header show")
    sw.run_capture("header", "mode_auto", "header mode auto")
    for deg in (90, 180, 270):
        sw.run_capture("header", "rotate_%d" % deg, "rotate %d" % deg)
        time.sleep(1.0)
        sw._snap("header", "rotate_%d_shot" % deg)
        sw.run_capture("header", "unrotate_%d" % deg, "rotate 0")
        time.sleep(1.0)


def keyboard_surfaces(sw: Sweep) -> None:
    sw.run_capture("keyboard", "show", "keyboard show")
    for page in ("text_lower", "text_upper", "number", "symbols", "nav", "nav2"):
        sw.run_capture("keyboard", "page_%s" % page, "keyboard mode %s" % page)
        sw._snap("keyboard", "page_%s_shot" % page)
    sw.run_capture("keyboard", "back_letters", "keyboard mode text_lower")
    sw.run_capture("keyboard", "nav_on", "keyboard nav on")
    sw._snap("keyboard", "nav_on_shot")
    sw.run_capture("keyboard", "nav_off", "keyboard nav off")
    sw.run_capture("keyboard", "hide", "keyboard hide")
    sw._snap("keyboard", "hide_shot")
    sw.run_capture("keyboard", "show2", "keyboard show")


def modal_surfaces(sw: Sweep) -> None:
    sw.dev.run("delay 1500", timeout=10)
    sw.modal_capture("modal", "dialog_ok_cancel",
                     'dialog /t:30 "Save changes?" "The document has unsaved changes." "Save" "Discard"',
                     answer="q")
    sw.modal_capture("modal", "dialog_yesno",
                     'dialog /t:30 "Confirm delete" "Delete all records?" yesno', answer="n")
    sw.modal_capture("modal", "list_short",
                     'list /t:30 "Pick an option" "alpha" "bravo" "charlie"', answer="q")
    items = " ".join('"item %02d"' % i for i in range(18))
    sw.modal_capture("modal", "list_long",
                     'list /t:30 "Long list" %s' % items, answer="q")
    sw.modal_capture("modal", "ask", 'ask /t:30 "Your name"', answer="tester")
    sw.modal_capture("modal", "ask_password", 'ask /t:30 /p "Passcode"', answer="secret")
    sw.modal_capture("modal", "browse", 'browse /t:30 sd:/APPS', answer="q")
    sw.modal_capture("modal", "form",
                     'form /t:30 "Preferences" "Theme=select:default|amber|ice|mono:theme" '
                     '"Lock=check:lock" "Name=text:name"',
                     answer="q")


def editor_surfaces(sw: Sweep) -> None:
    lines = "\r\n".join("line %02d  The quick brown fox jumps over the lazy dog." % i
                        for i in range(60)) + "\r\n"
    try:
        from p4test import sdbridge
        sdbridge.push_file(sw.dev.session, "P4VIS.TXT", lines.encode())
    except Exception as exc:  # noqa: BLE001
        print("   push P4VIS.TXT failed:", exc)
    sw.modal_capture("editor", "open", "edit P4VIS.TXT", answer="\\q")
    time.sleep(0.5)
    sw.dev.session.write_line("y")
    time.sleep(1.0)
    sw.dev.session.read_for(0.5)
    # unnamed buffer
    sw.modal_capture("editor", "unnamed", "edit", answer="\\q")
    time.sleep(0.5)
    sw.dev.session.write_line("y")
    time.sleep(1.0)
    sw.dev.session.read_for(0.5)


def tui_surfaces(sw: Sweep) -> None:
    sw.run_capture("tui", "status", "tui status")
    sw.run_capture("tui", "box_single",
                   'draw clear screen & draw box 2 2 30 8 single "SINGLE" & draw refresh')
    sw.run_capture("tui", "box_double",
                   'draw clear screen & draw box 2 2 30 8 double "DOUBLE" & draw refresh')
    sw.run_capture("tui", "box_rounded",
                   'draw clear screen & draw box 2 2 30 8 rounded "ROUND" & draw refresh')
    sw.run_capture("tui", "lines",
                   "draw clear screen & draw line 2 2 60 2 single & draw line 2 3 60 3 double & draw line 2 4 60 4 heavy & draw refresh")
    sw.run_capture("tui", "text_colors",
                   "draw clear screen & color 9 & locate 2 2 & echo red & color 12 & locate 2 3 & echo blue & color 7 & locate 2 4 & echo normal & draw refresh")
    sw.run_capture("tui", "fullscreen_on", "draw fullscreen on")
    sw.run_capture("tui", "fullscreen_box",
                   'draw clear screen & draw box 1 1 78 23 double "FULLSCREEN" & draw refresh')
    sw.run_capture("tui", "fullscreen_off", "draw fullscreen off")
    sw.run_capture("tui", "close", "draw close")


def gfx_plot_surfaces(sw: Sweep) -> None:
    sw.run_capture("gfx", "prim",
                   'gfx init 320 200 & gfx clear 0 & gfx line 2 2 300 2 15 & gfx rect 10 20 60 40 10 & gfx rect 90 20 60 40 12 fill & gfx circle 200 60 30 14 fill & gfx triangle 10 120 80 120 45 170 9 fill & gfx text 10 180 15 "GFX TEXT" & gfx show')
    sw.run_capture("gfx", "close", "gfx close")

    sw.run_capture("plot", "canvas",
                   'plot tui off & gfx init 320 200 & plot window -10 10 -10 10 & plot axes /grid & plot func "sin(X)" 15 & gfx show')
    sw.run_capture("plot", "tui", "plot tui on")
    sw.run_capture("plot", "tui_draw",
                   'plot window -10 10 -10 10 & plot axes /grid & plot func "sin(X)" 15 & draw refresh')
    sw.run_capture("plot", "tui_off", "plot tui off")
    sw.run_capture("plot", "close", "gfx close & draw close")


def _tcmd_capture(sw: Sweep, name: str = "tcmd_open", attempts: int = 3) -> Capture:
    """Capture TCMD via its own SD snapshot.

    TCMD's `choice` prompt owns the serial input, so an injected streaming
    `screenshot` is consumed by the app and the frame streams desynchronise
    (T1). Instead press TCMD's snapshot key ('p' -> saves TCMD.BMP) and pull
    the saved file after quitting. The snapshot can land on a mid-redraw frame,
    so retry until the pulled image is not blank.
    """
    session = sw.dev.session
    for attempt in range(attempts):
        try:
            sw.dev.run("draw close", timeout=10)
        except Exception:  # noqa: BLE001
            pass
        session.reset_input()
        session.write_line("launch TCMD")
        time.sleep(10.0)          # TCMD populates its panes a few seconds in
        for _ in range(4):        # several snaps so one lands after a redraw
            session.write_line("p")
            time.sleep(2.0)
        for _ in range(5):
            session.write(b"\x03\r\n")
            time.sleep(0.7)
            try:
                if "P4APPOK" in sw.dev.run("echo P4APPOK", timeout=12):
                    break
            except Exception:  # noqa: BLE001
                continue
        cap = sw._snap("apps", name,
                       notes="T1: TCMD snapshot+pull attempt %d" % (attempt + 1),
                       pulled_from="TCMD.BMP")
        ink = 0.0
        try:
            raw = open(cap.bmp, "rb").read()
            bmp = p4shot._parse_bmp(raw)
            data = raw[bmp.data_offset:]
            ink = sum(1 for b in data if b > 30) / max(1, len(data))
        except Exception:  # noqa: BLE001
            pass
        print("   [apps          ] %-28s pulled ink=%.4f" % (name, ink), flush=True)
        if ink > 0.004 or attempt == attempts - 1:
            if ink <= 0.004:
                cap.findings.append(Finding(
                    "LOW", "capture-failed", "apps", name,
                    "TCMD snapshot stayed blank after %d attempts" % attempts,
                    cap.png))
                sw.findings.extend(cap.findings)
            return cap
    return cap


def app_surfaces(sw: Sweep) -> None:
    """Launch every reference app, screenshot the opening surface, exit."""
    sw.dev.run("delay 1000", timeout=10)
    apps = [
        ("ADVENT", "quit"),
        ("NOTES", "8"),
        ("MOOD", "5"),
        ("TCMD", "q"),
        ("ELITE", "5"),
        ("PICS", "5"),
        ("PALMTOP", "7"),
        ("DIAG", "9"),
        ("COMPANION", "6"),
    ]
    for name, quit_key in apps:
        session = sw.dev.session
        if name == "TCMD":
            _tcmd_capture(sw)
            continue
        session.reset_input()
        session.write_line("launch %s" % name)
        time.sleep(4.0)
        sw._snap("apps", name.lower() + "_open")
        session.read_for(1.0)
        # Escape whatever surface the app owns (foreground break + modal close)
        # and prove the shell is back before the next launch.
        for _ in range(5):
            session.write(b"\x03\r\n")
            time.sleep(0.7)
            try:
                out = sw.dev.run("echo P4APPOK", timeout=12)
                if "P4APPOK" in out:
                    break
            except Exception:  # noqa: BLE001
                continue
        try:
            sw.dev.run("draw close", timeout=10)
        except Exception:  # noqa: BLE001
            pass


# --------------------------------------------------------------------------
# Contact sheet
# --------------------------------------------------------------------------

def contact_sheet(paths: List[str], out_path: str, cols: int = 5,
                  thumb_w: int = 400) -> None:
    try:
        from PIL import Image, ImageDraw
    except Exception:  # noqa: BLE001
        return
    images = []
    for p in paths:
        png = p[:-4] + ".png" if p.endswith(".bmp") else p
        if not os.path.exists(png):
            continue
        try:
            im = Image.open(png)
        except Exception:  # noqa: BLE001
            continue
        im = im.convert("RGB")
        ratio = thumb_w / im.width
        images.append((os.path.basename(png), im.resize((thumb_w, int(im.height * ratio)))))
    if not images:
        return
    th = max(im.height for _, im in images)
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (thumb_w + 6) + 6, rows * (th + 24) + 6), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for idx, (name, im) in enumerate(images):
        cx = 6 + (idx % cols) * (thumb_w + 6)
        cy = 6 + (idx // cols) * (th + 24)
        sheet.paste(im, (cx, cy))
        draw.text((cx + 2, cy + im.height + 4), name[:48], fill=(220, 220, 220))
    sheet.save(out_path)
    print("contact sheet:", out_path)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="P4MiniShell visual-accuracy sweep")
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("--only", default=None, help="comma-separated groups")
    ap.add_argument("--no-apps", action="store_true")
    ap.add_argument("--quick", action="store_true", help="fewer surfaces")
    ap.add_argument("--out", default=None,
                    help="output dir (default screenshots/visual/<PORT>)")
    args = ap.parse_args(argv)

    # Namespace the output by port so two boards can be swept in parallel
    # without overwriting each other's captures/report.
    out_dir = args.out or (
        os.path.join(OUT, args.port.upper()) if args.port else OUT)

    groups = {
        "shell": shell_surfaces,
        "header": header_surfaces,
        "keyboard": keyboard_surfaces,
        "modal": modal_surfaces,
        "editor": editor_surfaces,
        "tui": tui_surfaces,
        "gfxplot": gfx_plot_surfaces,
        "apps": app_surfaces,
    }
    if args.only:
        want = [g.strip() for g in args.only.split(",")]
        groups = {k: v for k, v in groups.items() if k in want}
    if args.no_apps:
        groups.pop("apps", None)

    print("visual sweep: port=%s groups=%s" % (args.port or "default",
                                               ",".join(groups)))
    dev = Device(args.port, boot=True, settle_ms=6000)
    # Quiesce the boot Wi-Fi bring-up: while it runs the LVGL task is starved
    # and the first painted frame of a new surface can lag by seconds.
    try:
        dev.run("delay 2000", timeout=10)
        for _ in range(20):
            if "connected: yes" in dev.run("wifi status", timeout=25):
                break
            time.sleep(2)
    except Exception:  # noqa: BLE001
        pass
    sw = Sweep(dev, out_dir)
    try:
        for gname, fn in groups.items():
            print("== group: %s ==" % gname, flush=True)
            try:
                fn(sw)
            except PanicError:
                raise
            except Exception as exc:  # noqa: BLE001
                print("   group %s raised: %s" % (gname, exc))
    finally:
        dev.close()

    # Report
    report = {
        "port": args.port,
        "captures": [
            {"group": c.group, "name": c.name, "png": c.png,
             "width": c.width, "height": c.height,
             "findings": [f.__dict__ for f in c.findings]}
            for c in sw.captures
        ],
        "findings": [f.__dict__ for f in sw.findings],
    }
    with open(os.path.join(sw.out, "report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2, ensure_ascii=False)

    # Review draft
    with open(os.path.join(sw.out, "findings_draft.md"), "w", encoding="utf-8") as fh:
        fh.write("# Visual sweep - candidate findings\n\n")
        by_kind = {}
        for f in sw.findings:
            by_kind.setdefault(f.kind, []).append(f)
        for kind, items in sorted(by_kind.items()):
            fh.write("## %s (%d)\n\n" % (kind, len(items)))
            for f in items:
                fh.write("- [%s] %s/%s: %s\n  evidence: %s\n"
                         % (f.severity, f.group, f.name, f.detail, f.evidence))
            fh.write("\n")

    # Contact sheets per group
    for g in {c.group for c in sw.captures}:
        paths = [c.bmp for c in sw.captures if c.group == g]
        contact_sheet(paths, os.path.join(sw.out, "contact_%s.png" % g))

    print("\ncaptures=%d findings=%d" % (len(sw.captures), len(sw.findings)))
    print("report:", os.path.join(sw.out, "report.json"))
    print("draft :", os.path.join(sw.out, "findings_draft.md"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
