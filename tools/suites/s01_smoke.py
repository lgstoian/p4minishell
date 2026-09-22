# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Smoke suite: identity, help, memory, and a screenshot geometry check.

This is the first suite in every run: it proves the session, the marker-sync
runner, and the streaming screenshot path all work before the deeper suites.
"""
import os

from p4test.asserts import Checklist

NAME = "smoke"
TAGS = ["core", "fast"]
OUT_DIR = os.path.join("screenshots", "regression")


def run(dev, ctx):
    c = Checklist(NAME)

    # The board line reports the active profile's requested/detected names; match
    # the detected name the firmware itself advertises rather than a fixed
    # reference-board string so the suite runs on the Tab5 too.
    detected = ""
    for line in dev.sysinfo().splitlines():
        if line.strip().startswith("board.detected_name:"):
            detected = line.split(":", 1)[1].strip()
            break

    about_out = dev.run("about")
    c.expect("about banner", "P4MiniShell", about_out)
    # Firmware version floats with releases (1.2.0 -> 1.2.1 ...); match the
    # label the firmware itself prints rather than a hardcoded version.
    c.expect("about version", "about.version:", about_out)
    out = about_out
    if detected:
        c.expect("about board", detected, out)
    else:
        c.expect("about board", "about.board:", out)
    c.expect("about idf", "v5.5.5", out)

    out = dev.run("help")
    c.expect("help lists dir", "dir", out)
    c.expect("help lists config", "config", out)

    out = dev.run("mem")
    c.expect("mem reports heap", "heap", out)

    out = dev.run("version")
    # Expect the same version string `about` reported, not a hardcoded value.
    import re as _re
    _m = _re.search(r"about\.version:\s*(\S+)", about_out)
    _want = _m.group(1) if _m else "1."
    c.expect("version banner", _want, out)

    # The on-screen keyboard's visibility is persisted UI state, so pin it
    # visible before sampling; otherwise a prior `keyboard hide` (e.g. the
    # appdiff gate) makes this geometry check fail for unrelated reasons.
    dev.run("keyboard show", timeout=15)
    disp_w, disp_h = dev.display_size()
    bmp = dev.screenshot(out_dir=OUT_DIR, name="smoke")
    c.equals("screenshot width", bmp.width, disp_w)
    c.equals("screenshot height", bmp.height, disp_h)
    c.equals("screenshot bpp", bmp.bpp, 24)
    # The shell chrome is dark; the on-screen keyboard is light. Sample the
    # keyboard band (bottom quarter) and require some bright pixels.
    band_y = disp_h - disp_h // 4
    mean = bmp.region_mean(0, band_y, disp_w, disp_h - band_y)
    c.check("keyboard band is visible", sum(mean) > 90,
            "mean=%r" % (mean,))

    return c
