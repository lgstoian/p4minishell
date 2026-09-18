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

    out = dev.run("about")
    c.expect("about banner", "P4MiniShell", out)
    c.expect("about version", "about.version: 1.0.0", out)
    c.expect("about board", "JC1060P470C", out)
    c.expect("about idf", "v5.5.5", out)

    out = dev.run("help")
    c.expect("help lists dir", "dir", out)
    c.expect("help lists config", "config", out)

    out = dev.run("mem")
    c.expect("mem reports heap", "heap", out)

    out = dev.run("version")
    c.expect("version banner", "1.0.0", out)

    # The on-screen keyboard's visibility is persisted UI state, so pin it
    # visible before sampling; otherwise a prior `keyboard hide` (e.g. the
    # appdiff gate) makes this geometry check fail for unrelated reasons.
    dev.run("keyboard show", timeout=15)
    bmp = dev.screenshot(out_dir=OUT_DIR, name="smoke")
    c.equals("screenshot width", bmp.width, 1024)
    c.equals("screenshot height", bmp.height, 600)
    c.equals("screenshot bpp", bmp.bpp, 24)
    # The shell chrome is dark; the on-screen keyboard is light. Sample the
    # keyboard band (bottom third) and require some bright pixels.
    mean = bmp.region_mean(0, 450, 1024, 150)
    c.check("keyboard band is visible", sum(mean) > 90,
            "mean=%r" % (mean,))

    return c
