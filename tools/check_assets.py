#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Fail when a committed UI capture contains LVGL's default-theme colours.

The gallery and the shell idle shot are the screenshots users see in the docs,
and they are also the cheapest place to catch an unstyled widget: the default
theme's primary blue (``#2196F3``) is never part of this UI, so a sized blob of
it means a widget lost its theme style. That is exactly how the shell's
input-row buttons shipped as a bright blue band - a flash every time an app or
modal closed and the row reappeared.

Only the captures this project controls are scanned: ``docs/assets/apps/*.png``
and ``docs/assets/shell-idle.png``. Browser/wiki captures show third-party
pages and are deliberately out of scope.

Exit code 0 when clean, 1 when a capture contains a blob.
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from p4test import theme_guard  # noqa: E402

# Docs captures are high-resolution; a real unstyled button is thousands of
# pixels, an anti-aliasing fringe is a handful.
MIN_PIXELS = 200


def captures():
    paths = sorted(glob.glob(os.path.join(ROOT, "docs", "assets", "apps", "*.png")))
    idle = os.path.join(ROOT, "docs", "assets", "shell-idle.png")
    if os.path.exists(idle):
        paths.append(idle)
    return paths


def main():
    paths = captures()
    if not paths:
        print("check_assets: no UI captures found")
        return 0
    bad = 0
    for path in paths:
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        try:
            hits = theme_guard.scan_path(path, min_pixels=MIN_PIXELS)
        except Exception as exc:  # noqa: BLE001
            print("check_assets: %s: cannot read (%s)" % (rel, exc))
            bad += 1
            continue
        if hits:
            print("check_assets: %s: unstyled default-theme widget: %s"
                  % (rel, theme_guard.describe(hits)))
            bad += 1
    print("check_assets: %d capture(s), %d with default-theme blobs"
          % (len(paths), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
