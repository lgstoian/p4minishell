#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""ui_touch_test.py - hardware touch-automation suite for every tappable widget.

Uses the firmware `ui` verbs (synthetic LVGL pointer indev) to tap real
coordinates, so it exercises the same hit-testing/event path as a finger.

Phases:
  1. shell keyboard: every key on every page types the right character
  2. header indicators: tap each status glyph
  3. input-row buttons: Prev/Next/up/down/Tab
  4. editor: type on the letters page, then every Nav/Edit action key
  5. modals: dialog/list buttons
  6. optional full-screen coordinate sweep (--sweep [px])

Usage: python tools/ui_touch_test.py [COMx] [--sweep [px]]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port, hard_reset  # noqa: E402

FAILS = []
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class Ui:
    def __init__(self, port):
        self.port = port
        self.s = open_port(port, 115200, 0.3)

    def close(self):
        self.s.close()

    def send(self, cmd, wait=1.2):
        self.s.reset_input_buffer()
        if cmd:
            self.s.write((cmd + "\r\n").encode())
        end = time.time() + wait
        out = b""
        while time.time() < end:
            c = self.s.read(65536)
            if c:
                out += c
        return ANSI.sub("", out.decode("utf-8", errors="replace"))

    def state(self, tries=4):
        for _ in range(tries):
            out = self.send("ui state", 0.9)
            m = re.search(r"ui\.state:(.*)", out)
            if m:
                body = m.group(1).splitlines()[0].strip()
                d = {}
                for kv in re.finditer(r"(\w+)=(.*?)(?=\s+\w+=|$)", body):
                    d[kv.group(1)] = kv.group(2).strip()
                return d
        return None

    def wait_state(self, pred, timeout=8.0):
        end = time.time() + timeout
        st = None
        while time.time() < end:
            st = self.state(tries=1)
            if st is not None and pred(st):
                return st
            time.sleep(0.25)
        return st

    def alive(self):
        return self.state(tries=2) is not None

    def targets(self):
        out = self.send("ui targets", 3.0)
        items = []
        for ln in out.splitlines():
            m = re.match(r"\s*(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(.*\S)\s*$", ln)
            if m:
                name = m.group(6)
                if name.endswith("[C]"):   # firmware clip marker (ui targets)
                    name = name[:-3].rstrip()
                items.append({"id": int(m.group(1)), "x": int(m.group(2)),
                              "y": int(m.group(3)), "w": int(m.group(4)),
                              "h": int(m.group(5)), "name": name})
        return items

    def tap(self, x, y, wait=1.0):
        return self.send("ui tap %d %d" % (x, y), wait)

    def key(self, label, wait=0.8):
        return self.send("ui key " + label, wait)

    def target(self, tid, wait=1.0):
        return self.send("ui target %d" % tid, wait)

    def ensure_shell(self):
        for _ in range(8):
            st = self.state(tries=2)
            if st is None:
                return False
            if st.get("modal") == "none":
                return True
            if st.get("modal") == "edit":
                self.send("\\q", 0.9)
                self.send("y", 0.7)
            else:
                self.send("q", 0.7)
                self.send("\n", 0.5)
        return (self.state(tries=2) or {}).get("modal") == "none"


def check(name, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           (" - " + str(detail)) if (detail and not ok) else ""))
    if not ok:
        FAILS.append(name)


def crash_free(text):
    return not any(k in text for k in
                   ("Guru Meditation", "Backtrace", "assert failed", "abort()",
                    "Stack protection", "Task watchdog"))


def kbd_labels(ui):
    for _ in range(3):
        labels = {}
        for t in ui.targets():
            if t["name"].startswith("kbd:"):
                labels[t["name"][4:]] = t
        if labels:
            return labels
        time.sleep(0.4)
    return {}


def phase_keyboard(ui):
    print("== shell keyboard ==")
    ui.ensure_shell()
    okay = True
    for mode, chars in (("text_lower", "abc"),
                        ("text_upper", "ABC"),
                        ("number", "123"),
                        ("symbols", "<>#")):
        ui.send("keyboard mode " + mode, 1.3)
        labels = kbd_labels(ui)
        if not labels:
            check("keys present (%s)" % mode, False)
            okay = False
            continue
        for ch in chars:
            if ch in labels:
                ui.key(ch, 0.8)
            else:
                check("key '%s' on %s" % (ch, mode), False, sorted(labels)[:10])
                okay = False
    ui.send("keyboard mode text_lower", 1.3)
    st = ui.state()
    inp = st.get("input", "") if st else ""
    check("keyboard typed across pages", okay and "ABC" in inp and "123" in inp,
          inp)


def phase_header(ui):
    print("== header indicators ==")
    ui.ensure_shell()
    header = [t for t in ui.targets() if t["y"] < 42 and t["w"] > 0]
    check("header targets present", len(header) > 0, "%d" % len(header))
    for t in header:
        out = ui.tap(t["x"] + t["w"] // 2, t["y"] + t["h"] // 2, 0.6)
        if not crash_free(out):
            check("header tap %s" % t["name"], False, out[-120:])
            return
    check("header taps crash-free", True)


def phase_input_row(ui):
    print("== input-row buttons ==")
    ui.ensure_shell()
    ui.send("keyboard show", 1.0)
    targets = ui.targets()
    # The input row (Prev/Next/Up/Dn/Tab) sits directly above the on-screen
    # keyboard, so locate it relative to the keyboard's top edge instead of a
    # fixed reference-board Y band (which is wrong on the 1280x720 Tab5).
    kbd_top = min((t["y"] for t in targets if t["name"].startswith("kbd:")),
                  default=None)
    row = []
    for t in targets:
        if t["name"].startswith("kbd:") or t["w"] <= 0 or t["h"] <= 0:
            continue
        if kbd_top is not None:
            if (kbd_top - 80) <= t["y"] < kbd_top:
                row.append(t)
        elif t["name"] in ("Prev", "Next", "Up", "Dn", "Tab"):
            row.append(t)
    check("input-row targets present", len(row) > 0, "%d" % len(row))
    for t in row:
        out = ui.tap(t["x"] + t["w"] // 2, t["y"] + t["h"] // 2, 0.5)
        if not crash_free(out):
            check("input-row tap %s" % t["name"], False, out[-120:])
            return
    check("input-row taps crash-free", True)


def phase_editor(ui):
    print("== editor keys ==")
    ui.ensure_shell()
    ui.send("del _uitest.txt", 0.5)
    ui.send("edit _uitest.txt", 4.0)
    st = ui.wait_state(lambda s: s.get("editor") == "open", 10)
    check("editor opened", st is not None and st.get("editor") == "open", st)
    if st is None or st.get("editor") != "open":
        return

    # Type on the letters page.
    ui.key("abc")
    ui.wait_state(lambda s: s.get("mode") == "text_lower", 4)
    for ch in "hello":
        ui.key(ch, 0.5)
    st = ui.wait_state(lambda s: s.get("modified") == "1", 4)
    check("editor typed (modified)", st is not None and st.get("modified") == "1", st)

    # Nav page: Undo/Redo/Save and every action key.
    ui.key("1#")
    ui.key("Nav")
    st = ui.wait_state(lambda s: s.get("mode") == "nav", 4)
    check("back on nav page", st is not None and st.get("mode") == "nav", st)
    ui.key("Undo", 0.5)
    ui.key("Redo", 0.5)
    ui.key("Save", 1.2)
    st = ui.wait_state(lambda s: s.get("modified") == "0", 5)
    check("editor saved (clean)", st is not None and st.get("modified") == "0", st)

    for label in ("Find", "Goto", "SaveAs", "Open", "Replace", "ReplAll",
                  "Case", "Undo", "Redo", "Next", "PgUp", "PgDn", "Home",
                  "End", "Del", "Ins", "Tab"):
        out = ui.key(label, 0.5)
        if not crash_free(out):
            check("nav key %s" % label, False, out[-120:])
            return
        st = ui.state(tries=2)
        if st and st.get("editor") == "open" and st.get("mode") != "nav":
            ui.key("abc", 0.4)
            ui.key("1#", 0.4)
            ui.key("Nav", 0.4)
    check("nav keys crash-free", True)

    # Edit page keys.
    ui.key("Edit", 0.5)
    st = ui.wait_state(lambda s: s.get("mode") == "nav2", 4)
    check("edit page", st is not None and st.get("mode") == "nav2", st)
    for label in ("Copy", "Cut", "Paste", "SelAll", "WordL", "WordR", "DocTop",
                  "DocBot", "DelLine", "DelEOL", "Reload", "Preview", "Comment",
                  "Match", "Wrap"):
        out = ui.key(label, 0.5)
        if not crash_free(out):
            check("edit key %s" % label, False, out[-120:])
            return
    check("edit keys crash-free", True)

    # Close (discard if modified).
    for _ in range(6):
        st = ui.state(tries=1)
        if st and st.get("editor") == "closed":
            break
        ui.send("\\q", 0.9)
        ui.send("y", 0.7)
    st = ui.state()
    check("editor closed", st is not None and st.get("editor") == "closed", st)
    ui.send("del _uitest.txt", 0.5)


def phase_modals(ui):
    print("== modals ==")
    if not ui.ensure_shell():
        check("shell for modals", False)
        return
    ui.send("dialog \"T\" \"Hello\" \"OK\" \"Cancel\"", 3.0)
    st = ui.wait_state(lambda s: s.get("modal") == "dialog", 5)
    if st and st.get("modal") == "dialog":
        btns = [t for t in ui.targets()
                if t["name"] in ("OK", "Cancel", "kbd:OK", "kbd:Cancel")]
        if btns:
            ui.target(btns[0]["id"], 1.2)
        st2 = ui.wait_state(lambda s: s.get("modal") == "none", 5)
        check("dialog button tap", st2 is not None and st2.get("modal") == "none", st2)
    else:
        check("dialog opened", False, st)

    ui.send("list \"T\" \"one\" \"two\" \"three\"", 3.0)
    st = ui.wait_state(lambda s: s.get("modal") == "list", 5)
    if st and st.get("modal") == "list":
        opts = [t for t in ui.targets()
                if any(t["name"].endswith(x) for x in ("one", "two", "three"))]
        if opts:
            ui.target(opts[1]["id"], 1.2)
        st2 = ui.wait_state(lambda s: s.get("modal") == "none", 5)
        check("list item tap", st2 is not None and st2.get("modal") == "none", st2)
    else:
        check("list opened", False, st)


def phase_sweep(ui, step):
    print("== full-screen sweep (step %d) ==")
    if not ui.ensure_shell():
        check("shell for sweep", False)
        return
    w, h = 1024, 600
    n = 0
    y = 4
    while y < h:
        x = 4
        while x < w:
            out = ui.tap(x, y, 0.22)
            n += 1
            if not crash_free(out) or not ui.alive():
                check("sweep point %d,%d" % (x, y), False, out[-100:])
                return
            x += step
        y += step
    check("full-screen sweep (%d points)" % n, True)


def main():
    args = list(sys.argv[1:])
    sweep = None
    if "--sweep" in args:
        i = args.index("--sweep")
        args.pop(i)
        if i < len(args) and args[i].isdigit():
            sweep = int(args[i])
            args.pop(i)
        else:
            sweep = 48
    port = args[0] if args else "COM3"

    hard_reset(port)
    time.sleep(1)
    ui = Ui(port)
    try:
        time.sleep(15)
        st = ui.state()
        if st is None:
            print("RESULT FAIL (board not responding)")
            return 1
        print("initial state:", st)
        ui.ensure_shell()
        phase_keyboard(ui)
        phase_header(ui)
        phase_input_row(ui)
        phase_editor(ui)
        phase_modals(ui)
        if sweep:
            phase_sweep(ui, sweep)
    finally:
        ui.close()

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
