#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gen_apps_gallery.py - build docs/apps.md from the captured app screenshots.

The screenshots are produced by `tools/suites/s25_apps_gallery.py` (hardware);
this tool only writes the Markdown index, reading each app's title/description
from its APPINFO so the gallery cannot drift from the app list.

Usage:
    python tools/gen_apps_gallery.py [--assets docs/assets/apps]
                                     [--out docs/apps.md]
"""
from __future__ import annotations

import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def appinfo_for(stem: str) -> tuple[str, str]:
    """Find `<STEM>.APPINFO` anywhere under apps/ and return (title, desc)."""
    want = stem.upper() + ".APPINFO"
    for base, _dirs, files in os.walk(os.path.join(ROOT, "apps")):
        for name in files:
            if name.upper() != want:
                continue
            title = desc = ""
            with open(os.path.join(base, name), encoding="utf-8",
                      errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if line.startswith("title="):
                        title = line[6:].strip()
                    elif line.startswith("description="):
                        desc = line[12:].strip()
            return title, desc
    return "", ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.join(ROOT, "docs", "assets", "apps"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "apps.md"))
    args = ap.parse_args()

    shots = sorted(n for n in os.listdir(args.assets) if n.lower().endswith(".png")) \
        if os.path.isdir(args.assets) else []
    if not shots:
        print("no screenshots in", args.assets)
        return 1

    lines = [
        "# Reference app gallery",
        "",
        "Every screenshot below is captured live from the device by",
        "[`tools/suites/s25_apps_gallery.py`](../tools/suites/s25_apps_gallery.py)",
        "(JC1060P470C, 1024x600). The apps are batch-first and run from SD or USB;",
        "metadata comes from each app's `APPINFO`.",
        "",
        "The feature matrix in [`readme.md`](../readme.md) lists which v1.4.3",
        "surface each app exercises (emoji, transparency, HTML, drive-aware data).",
        "",
    ]
    for shot in shots:
        stem = os.path.splitext(shot)[0]
        title, desc = appinfo_for(stem)
        title = title or stem.upper()
        lines.append("## %s" % title)
        lines.append("")
        lines.append("![%s](assets/apps/%s)" % (title, shot))
        lines.append("")
        if desc:
            lines.append(desc)
            lines.append("")
        lines.append("*Screenshot: `docs/assets/apps/%s`*" % shot)
        lines.append("")
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print("wrote %s (%d apps)" % (args.out, len(shots)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
