# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""App feature-probe suite: every entry app exercises the new surfaces.

The apps call ``gosub APPLIB.BAT::probe`` at startup. The probe is opt-in via
``P4_APP_FEATURES=1`` so a normal user's transcript/pace is unchanged; with it
set, the app runs the locale formatter, the JS engine, the ``browser`` verb and
the persisted HTTP cookie jar, emitting ``[M-FEAT-*]`` markers.

This suite:

* pushes APPLIB.BAT plus a few self-terminating / menu apps from the repo (so
  it can run standalone, before or after s14_apps),
* sets the opt-in flag and drives HYBRID (nonstop), CHAT (menu quit) and one
  game/demo (PLOT) to prove the probe runs in every app shape,
* checks locale persistence through a real reboot (`config LOCALE=DE` ->
  reboot -> `locale` -> restore US),
* checks the JS and browser verb surfaces directly.

The reboot leg is deliberately last: it clears the opt-in environment variable.
"""

import os
import time

from p4test import Checklist

NAME = "app_features"
TAGS = ["apps", "slow"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")

FEATURE_MARKERS = [
    "[M-FEAT]",
    "[M-FEAT-LOCALE]",
    "[M-FEAT-JS]",
    "[M-FEAT-BROWSER]",
    "[M-FEAT-COOKIE]",
    "[M-FEAT-DONE]",
]


def _push(dev, subdir, name, remote=None):
    path = os.path.join(APPS, subdir, name) if subdir else os.path.join(APPS, name)
    with open(path, "rb") as f:
        data = f.read()
    dev.push_file(remote or name, data)


def _expect_features(c, app, out):
    for marker in FEATURE_MARKERS:
        c.expect("%s emitted %s" % (app, marker), marker, out)


def run(dev, ctx):
    c = Checklist(NAME)

    _push(dev, "", "APPLIB.BAT")
    _push(dev, "hybrid", "HYBRID.BAT")
    _push(dev, "chat", "CHAT.BAT")
    _push(dev, "gfxdemo", "PLOT.BAT")

    # -- opt-in probe runs inside a nonstop app -----------------------------
    dev.run("set P4_APP_FEATURES=1", timeout=15)
    out = dev.run("launch HYBRID", timeout=60, settle=1.0)
    c.expect("HYBRID started", "[M-HYBRID]", out)
    c.expect("HYBRID finished", "[M-HYBRID-DONE]", out)
    _expect_features(c, "HYBRID", out)

    # -- probe runs in a menu app too, without disturbing its exit ----------
    dev.session.write_line("launch CHAT")
    head = dev.session.read_until(b"[M-CHAT]", 60)
    c.expect("CHAT start marker", "[M-CHAT]", head.decode("utf-8", "replace"))
    head_text = head.decode("utf-8", "replace")
    _expect_features(c, "CHAT", head_text)
    # The marker is emitted before the probe and the menu render; give the
    # `list` widget time to own the input before selecting Quit.
    time.sleep(3.0)
    dev.session.write_line("9")
    tail = dev.session.read_until(b"[M-CHAT-DONE]", 40)
    if b"[M-CHAT-DONE]" not in tail:
        dev.session.write_line("9")
        tail += dev.session.read_until(b"[M-CHAT-DONE]", 20)
    c.expect("CHAT exit marker", "[M-CHAT-DONE]", tail.decode("utf-8", "replace"))

    # -- probe runs in a demo/game-shaped nonstop app -----------------------
    out = dev.run("launch PLOT", timeout=70, settle=1.0)
    c.expect("PLOT started", "[M-PLOT]", out)
    c.expect("PLOT finished", "[M-PLOT-DONE]", out)
    _expect_features(c, "PLOT", out)

    # -- direct verb surface (independent of the probe) ---------------------
    out = dev.run("js eval \"print('[M-JS-DIRECT]')\"", timeout=20)
    c.expect("js eval runs", "[M-JS-DIRECT]", out)

    dev.push_file("JSFEAT.JS", b"print('[M-JS-RUN]');\n")
    out = dev.run("js run JSFEAT.JS", timeout=20)
    c.expect("js run executes the file", "[M-JS-RUN]", out)
    dev.run("del /f JSFEAT.JS", timeout=30)

    out = dev.run("browser", timeout=15)
    c.expect("browser usage", "browser <url>", out)

    out = dev.run("httpget /cookie", timeout=15)
    c.check("cookie jar responds", "no stored cookies" in out or "=" in out,
            out[-200:])

    dev.run("set P4_APP_FEATURES=", timeout=15)

    # -- locale persistence through a real reboot --------------------------
    out = dev.run("config LOCALE=DE", timeout=20)
    c.expect("locale saved", "saved", out)
    dev.reboot()
    time.sleep(3.0)
    out = dev.run("locale", timeout=25)
    c.expect("locale survived reboot", "locale: DE", out)
    out = dev.run("config LOCALE=US", timeout=20)
    c.expect("locale restored", "saved", out)

    return c
