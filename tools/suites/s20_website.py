# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Browser showcase suite: render a real website entirely on the device.

The site lives in ``apps/website/`` (``INDEX.HTML`` + ``PAGE2.HTML``) and is
rendered by the ``browser`` verb from ``sd:/WEB/INDEX.HTML``. A non-http(s)
argument is a local path, so this needs no AP, cloud or httpd - the deterministic
path is the showcase. Network URLs still work and are covered by s17/s18.

This suite is also the screenshot source for the public showcase
(``docs/assets/browser-website.png``): it captures while the page is held.
"""

import os
import time

from p4test import Checklist

NAME = "website"
TAGS = ["browser", "ui", "apps"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
OUT_DIR = os.path.join("screenshots", "regression")


def _push(dev, subdir, name, remote):
    path = os.path.join(APPS, subdir, name) if subdir else os.path.join(APPS, name)
    with open(path, "rb") as f:
        dev.push_file(remote, f.read())


def run(dev, ctx):
    c = Checklist(NAME)

    dev.run("md WEB", timeout=20)
    _push(dev, "website", "INDEX.HTML", "WEB/INDEX.HTML")
    _push(dev, "website", "PAGE2.HTML", "WEB/PAGE2.HTML")
    _push(dev, "website", "WEB.BAT", "WEB.BAT")
    _push(dev, "", "APPLIB.BAT", "APPLIB.BAT")

    out = dev.run("dir WEB/INDEX.HTML", timeout=20)
    c.expect("website fixture deployed", "INDEX.HTML", out)

    dev.session.reset_input()
    dev.session.write_line("launch WEB")
    head = dev.session.read_until(b"[M-WEB]", 60)
    c.expect("WEB start marker", "[M-WEB]", head.decode("utf-8", "replace"))

    tail = dev.session.read_until(b"[M-WEB-LOADED]", 60)
    text = tail.decode("utf-8", "replace")
    c.expect("page rendered", "[M-WEB-LOADED]", text)

    # The app renders, captures the framebuffer to WEB/SHOT.BMP, then reports
    # done. Pull that BMP (rather than streaming) so the capture happens while
    # the page is still on screen.
    rest = dev.session.read_until(b"[M-WEB-DONE]", 90)
    rest_text = rest.decode("utf-8", "replace")
    c.expect("WEB finished", "[M-WEB-DONE]", rest_text)
    c.check("page render did not fail", "[M-WEB-FAIL]" not in rest_text,
            rest_text[-300:])

    if "[M-WEB-DONE]" in rest_text and "[M-WEB-FAIL]" not in rest_text:
        try:
            data = dev.pull_file("WEB/SHOT.BMP")
            os.makedirs(OUT_DIR, exist_ok=True)
            bmp_path = os.path.join(OUT_DIR, "browser-website.bmp")
            with open(bmp_path, "wb") as f:
                f.write(data)
            c.check("showcase screenshot captured", len(data) > 1000,
                    "%d bytes (PNG conversion is a docs step)" % len(data))
        except Exception as exc:  # noqa: BLE001
            c.check("showcase screenshot captured", False, str(exc))

    dev.run("del /f WEB/SHOT.BMP", timeout=30)
    return c
