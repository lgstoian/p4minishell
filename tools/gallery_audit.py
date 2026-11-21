#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Catch the false green this harness shipped: gallery PNGs that look plausible
but show the shell (or another app) instead of the app they are named after.

The old gate only asked "valid PNG, right size, some ink, >=3 colours". Fourteen
of the shipped app images were the SAME shell transcript dump with BOUNCE frame
echoes and passed that gate. This tool adds two independent checks:

  * identity by exclusion: a full-screen app capture must not contain the
    shell's header/prompt/keyboard chrome at the bottom of the panel;
  * uniqueness: two apps may not publish (nearly) the same pixels.

It is deterministic, takes well under a second, and runs on the committed PNGs
(so it cannot be fooled by what happened to be on the glass at test time).
"""
from __future__ import annotations

import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GALLERY = os.path.join(ROOT, "docs", "assets", "apps")

# Bottom strip of the reference panel: the shell prompt/keyboard row. Full-screen
# app pages leave it black. Measured on the shipped set: shell captures have
# ~9 colour buckets and >=4% bright pixels there, app pages have 1 bucket and 0%.
CHROME_TOP_FRACTION = 0.92
CHROME_BRIGHT = 0.02
CHROME_COLOURS = 4

# Two 24x24 grayscale signatures this close are the same screen (the ones that
# shipped as 'agenda' and 'aliases' are pixel-identical).
SIGNATURE_GRID = 24
DUPLICATE_L1 = 8


def signature(path: str):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    small = im.resize((SIGNATURE_GRID, SIGNATURE_GRID), Image.BOX).convert("L")
    sig = tuple(small.tobytes())
    px = im.load()
    bright = 0
    total = 0
    colours = set()
    for y in range(int(h * CHROME_TOP_FRACTION), h - 4, 2):
        for x in range(0, w, 4):
            r, g, b = px[x, y]
            total += 1
            if r + g + b > 250:
                bright += 1
            if len(colours) < 32:
                colours.add((r // 32, g // 32, b // 32))
    chrome = (total and bright / float(total) >= CHROME_BRIGHT and
              len(colours) >= CHROME_COLOURS)
    return im.size, sig, bool(chrome)


def audit(folder: str = GALLERY):
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".png"))
    entries = []
    for f in files:
        path = os.path.join(folder, f)
        try:
            size, sig, chrome = signature(path)
        except Exception as exc:  # noqa: BLE001
            print("  [FAIL] %s: undecodable: %s" % (f, exc))
            entries.append((f, None, False))
            continue
        entries.append((f, sig, chrome))
        if chrome:
            print("  [FAIL] %s: shows the shell prompt/keyboard, not an app page"
                  % f)

    duplicates = []
    for i, (fa, sa, _) in enumerate(entries):
        if sa is None:
            continue
        for fb, sb, _ in entries[i + 1:]:
            if sb is None:
                continue
            dist = sum(abs(a - b) for a, b in zip(sa, sb))
            if dist <= DUPLICATE_L1:
                duplicates.append((fa, fb, dist))
    for fa, fb, dist in duplicates:
        print("  [FAIL] %s and %s are the same screen (L1=%d)" % (fa, fb, dist))

    bad_chrome = sum(1 for _, _, chrome in entries if chrome)
    print("gallery_audit: %d PNG(s), %d shell capture(s), %d duplicate pair(s)"
          % (len(entries), bad_chrome, len(duplicates)))
    return 1 if (bad_chrome or duplicates or
                 any(s is None for _, s, _ in entries)) else 0


if __name__ == "__main__":
    sys.exit(audit(sys.argv[1] if len(sys.argv) > 1 else GALLERY))
