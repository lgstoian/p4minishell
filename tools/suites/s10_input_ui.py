# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Input/UI suite: on-screen keyboard, synthetic touch, and UI inspection.

Covers the ``keyboard`` page registry and visibility, the ``ui`` verbs
(``state``/``targets``/``hit``/``tap``/``key``/``target``), and the modal
activation path. Semantic ``ui target <id>`` is used for modal panels (a
coordinate tap into the auto-scrolling transcript is racy), while ``ui tap``
is exercised against a stable header target. Screenshots bracket the keyboard
show/hide transition.
"""
import os
import re
import time

from p4test import screenshot as shot
from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "input-ui"
TAGS = ["ui", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

TARGET_RE = re.compile(r"^\s*(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(.*\S)\s*$")

PAGES = {
    "text_lower": "text_lower",
    "text_upper": "text_upper",
    "number": "number",
    "symbols": "symbols",
    "nav": "nav",
    "nav2": "nav2",
}
ALIASES = {
    "letters": "text_lower",
    "caps": "text_upper",
    "num": "number",
    "special": "symbols",
    "edit": "nav2",
}


def parse_targets(text):
    out = []
    for ln in text.splitlines():
        m = TARGET_RE.match(ln)
        if m:
            name = m.group(6) or ""
            if name.endswith("[C]"):   # firmware clip marker (ui targets)
                name = name[:-3].rstrip()
            out.append({"id": int(m.group(1)), "x": int(m.group(2)),
                        "y": int(m.group(3)), "w": int(m.group(4)),
                        "h": int(m.group(5)), "name": name})
    return out


def _field(text, key):
    m = re.search(r"\b%s=(\S*)" % re.escape(key), text)
    return m.group(1) if m else None


def _between(text, key, nextkey):
    m = re.search(r"\b%s=(.*?)\s+%s=" % (re.escape(key), re.escape(nextkey)), text)
    return m.group(1) if m else None


def _console(dev, line, settle=1.0):
    """Send a console-reader line and return only its own reply."""
    dev.session.reset_input()
    dev.session.write_line(line)
    return dev.read_for(settle)


def _ui_state(dev, settle=1.0):
    return _console(dev, "ui state", settle)


def _wait_modal(dev, want, timeout=8.0):
    end = time.time() + timeout
    st = None
    while time.time() < end:
        st = _field(_ui_state(dev), "modal")
        if st == want:
            return st
        time.sleep(0.25)
    return st


def run(dev, ctx):
    c = Checklist(NAME)

    def grab(name):
        try:
            return dev.screenshot(out_dir=OUT_DIR, name=name)
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.note("screenshot %s failed: %s" % (name, exc))
            return None

    try:
        # =================================================================
        # 1. keyboard verb / page registry
        # =================================================================
        c.note("keyboard verbs")
        st = dev.run("keyboard status")
        c.expect("keyboard status", "keyboard:", st)
        c.expect("keyboard external field", "external=", st)
        c.expect("keyboard default page", "keyboard.page=text_lower",
                 dev.run("keyboard mode"))

        for page in PAGES:
            c.expect("keyboard page %s" % page,
                     "keyboard.page=%s" % PAGES[page],
                     dev.run("keyboard mode %s" % page))
        for alias, page in ALIASES.items():
            c.expect("keyboard alias %s" % alias,
                     "keyboard.page=%s" % page,
                     dev.run("keyboard mode %s" % alias))
        c.expect("keyboard reject unknown", "unknown page",
                 dev.run("keyboard mode s10-bogus"))
        c.expect("keyboard restore letters", "keyboard.page=text_lower",
                 dev.run("keyboard mode text_lower"))

        c.expect("keyboard nav state", "keyboard.nav=", dev.run("keyboard nav"))
        c.expect("keyboard nav on", "keyboard.nav=on", dev.run("keyboard nav on"))
        c.expect("keyboard nav off", "keyboard.nav=off", dev.run("keyboard nav off"))

        c.expect("keyboard show", "keyboard shown", dev.run("keyboard show"))
        c.expect("keyboard hide", "keyboard hidden", dev.run("keyboard hide"))
        dev.run("keyboard show")

        # =================================================================
        # 2. keyboard visibility screenshots
        # =================================================================
        c.note("keyboard visibility screenshots")
        dev.run("keyboard show")
        before = grab("s10_kbd_visible")
        dev.run("keyboard hide")
        after = grab("s10_kbd_hidden")
        if before is not None and after is not None:
            c.equals("keyboard screenshot width", before.width, 1024)
            c.equals("keyboard screenshot height", before.height, 600)
            ratio = shot.diff_ratio(before, after)
            c.check("keyboard hide changes screen", ratio > 0.02, "diff=%.3f" % ratio)
            band_before = sum(before.region_mean(0, 450, 1024, 150)) / 3.0
            band_after = sum(after.region_mean(0, 450, 1024, 150)) / 3.0
            c.check("keyboard band differs", abs(band_before - band_after) > 1.0,
                    "before=%.1f after=%.1f" % (band_before, band_after))
        else:
            c.check("keyboard screenshots captured", False, "screenshot failed")
        dev.run("keyboard show")

        # =================================================================
        # 3. ui state / targets / hit / tap
        # =================================================================
        c.note("ui state / targets / hit / tap")
        state = dev.run("ui state")
        c.expect("ui state prefix", "ui.state:", state)
        for key in ("modal=", "keyboard=", "mode=", "editor=", "modified=",
                    "row=", "col=", "lines=", "nav=", "path=", "input=",
                    "ghost=", "search="):
            c.expect("ui state field %s" % key, key, state)
        c.expect("ui state shell modal", "modal=none", state)
        c.expect("ui state editor closed", "editor=closed", state)

        bare = dev.run("ui state /b")
        c.check("ui state bare has no prefix", "ui.state:" not in bare and "modal=" in bare,
                bare[-200:])

        dev.run("ui state /v:S10S")
        # Inspect the stored variable with `set` rather than `echo %S10S%`: the
        # state text contains `input=PS /sdcard>` and re-expanding it is parsed
        # as output redirection (DOS semantics), so echo would print nothing.
        c.expect("ui state result var", "modal=", dev.run("set S10S"))

        targets = parse_targets(dev.run("ui targets", timeout=20))
        c.check("ui targets listed", len(targets) > 0, "%d targets" % len(targets))
        kbd = [t for t in targets if t["name"].startswith("kbd:")]
        c.check("ui targets include keyboard keys", len(kbd) > 0,
                "%d kbd targets" % len(kbd))

        if targets:
            t0 = targets[0]
            hit = dev.run("ui hit %d %d" % (t0["x"] + t0["w"] // 2,
                                             t0["y"] + t0["h"] // 2))
            c.expect("ui hit reports target", "ui.hit:", hit)

        header = [t for t in targets if t["y"] < 42 and t["w"] > 0]
        if header:
            t = header[0]
            out = dev.run("ui tap %d %d 80" % (t["x"] + t["w"] // 2,
                                                t["y"] + t["h"] // 2), timeout=15)
            c.check("ui tap header no panic", "Backtrace" not in out and "Guru" not in out)
            c.check("ui state alive after tap", _field(_ui_state(dev), "modal") is not None)
        else:
            c.check("header target present", False, "no header target")

        # =================================================================
        # 4. ui key -> input text + ghost completion
        # =================================================================
        c.note("ui key / ghost completion")
        dev.run("keyboard show")
        dev.run("keyboard mode text_lower")
        # Start from an empty input line so a stray character left by an earlier
        # check cannot make the "co" prefix assertion flake (H6). The on-screen
        # line is only cleared by submitting it (OSK enter key \uf8a2); a serial
        # empty line does not touch OSK-typed text, so it accumulated across
        # runs.
        dev.run("ui key \uf8a2", timeout=15)
        time.sleep(0.5)
        dev.session.reset_input()
        time.sleep(0.3)
        dev.run("ui key c", timeout=15)
        time.sleep(0.4)
        dev.run("ui key o", timeout=15)
        time.sleep(0.4)
        typed = _ui_state(dev)
        typed_input = _between(typed, "input", "ghost") or ""
        c.expect("ui key typed prefix", "co", typed_input)
        ghost = _between(typed, "ghost", "search") or ""
        c.check("ghost completion shown", len(ghost) > 0, "ghost=%r" % ghost)
        # Submit the pending input line (OSK enter) so the shell starts clean.
        dev.run("ui key \uf8a2", timeout=15)
        time.sleep(0.8)
        dev.read_for(0.5)

        # =================================================================
        # 5. Modal semantic activation (ui target)
        # =================================================================
        c.note("modal semantic activation")
        modal_before = grab("s10_modal_before")
        # /t:30 is a safety net: if semantic activation fails the dialog still
        # clears itself rather than blocking the following suites.
        dev.session.write_line('dialog /t:30 "S10" "hello" "OK" "Cancel"')
        st = _wait_modal(dev, "dialog", timeout=8)
        c.check("dialog opened", st == "dialog", "modal=%r" % st)
        modal_after = grab("s10_modal_open")
        if modal_before is not None and modal_after is not None:
            ratio = shot.diff_ratio(modal_before, modal_after)
            c.check("dialog changes screen", ratio > 0.01, "diff=%.3f" % ratio)

        if st == "dialog":
            dt = parse_targets(_console(dev, "ui targets", 3.0))
            btn = None
            for want in ("OK", "kbd:OK", "button"):
                for t in dt:
                    if t["name"] == want:
                        btn = t
                        break
                if btn is not None:
                    break
            if btn is not None:
                dev.send("ui target %d" % btn["id"], 1.5)
                c.check("dialog dismissed by target",
                        _wait_modal(dev, "none", timeout=6) == "none",
                        "modal=%r" % _ui_state(dev)[-160:])
            else:
                c.check("dialog action target found", False,
                        "targets=%r" % [t["name"] for t in dt][:20])
        else:
            c.check("dialog dismissed by target", False, "dialog did not open")

    finally:
        # Make sure no modal is left owning the screen, then restore the
        # baseline keyboard page/visibility.
        try:
            for _ in range(4):
                modal = _field(_ui_state(dev), "modal")
                if modal in (None, "none"):
                    break
                if modal == "edit":
                    dev.send("\\q", 0.9)
                    dev.send("y", 0.7)
                else:
                    dev.send("q", 0.7)
                    dev.session.write_line("")
                    time.sleep(0.3)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
        for cmd in ("keyboard show", "keyboard mode text_lower", "keyboard nav off"):
            try:
                dev.run(cmd, timeout=10)
            except PanicError:
                raise
            except Exception:  # noqa: BLE001
                pass

    return c
