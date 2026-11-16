#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""screenshot_gate.py - refresh and pixel-check the documentation screenshots.

Two modes:

  --update   drive the board through the curated states (tools/capture_docs.py)
             and write the PNGs to docs/assets/ (the new baseline).
  (default)  capture the same states into a temp directory and compare each
             frame with docs/assets/<name>.png, pixel for pixel.

The comparison is exact outside the masked regions. The header bar carries
live telemetry (heap, CPU, battery, link) and is masked by default; the clock
and the cursor are frozen by capture_docs.prelude(), so the rest of the frame
must match byte for byte.

A mismatch writes a diff image (differences painted red) to
screenshots/visual/<name>.diff.png and prints the count and bounding box, so a
rendering regression can be debugged rather than guessed at.

Usage:
  python tools/screenshot_gate.py --update --port COM3
  python tools/screenshot_gate.py --port COM3
  python tools/screenshot_gate.py --port COM3 --only shell-idle,tui-boxes
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import numpy as np
    from PIL import Image
except ImportError:  # pragma: no cover - documented prerequisite
    print("screenshot_gate: needs numpy and Pillow "
          "(python -m pip install numpy Pillow)", file=sys.stderr)
    raise SystemExit(2)

from p4test.device import Device  # noqa: E402
import capture_docs  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "docs", "assets")
MASKS = os.path.join(ROOT, "tools", "screenshot_masks.json")
DIFFS = os.path.join(ROOT, "screenshots", "visual")

# The header bar is the only part of the frame the UI cannot make
# deterministic (live heap/CPU/battery/link readouts). It is covered by the
# header-specific tests elsewhere; the docs gate masks it and compares the rest.
DEFAULT_MASK = [[0, 0, 4096, 34]]


def load_masks():
    if not os.path.isfile(MASKS):
        return {"default": DEFAULT_MASK, "shots": {}}
    with open(MASKS, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    doc.setdefault("default", DEFAULT_MASK)
    doc.setdefault("shots", {})
    return doc


def mask_for(name, doc, width, height):
    regions = list(doc.get("default", []))
    regions += list(doc.get("shots", {}).get(name, []))
    m = np.zeros((height, width), dtype=bool)
    for x0, y0, x1, y1 in regions:
        m[max(0, y0):min(height, y1), max(0, x0):min(width, x1)] = True
    return m


def load_rgb(path):
    with Image.open(path) as img:
        return np.asarray(img.convert("RGB"), dtype=np.uint8)


def diff_frames(base, cur, mask):
    h = min(base.shape[0], cur.shape[0])
    w = min(base.shape[1], cur.shape[1])
    b = base[:h, :w]
    c = cur[:h, :w]
    m = mask[:h, :w]
    different = np.any(b != c, axis=2) & ~m
    count = int(different.sum())
    if count == 0:
        return 0, None, None
    ys, xs = np.nonzero(different)
    bbox = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    return count, bbox, different


def write_diff(base, cur, different, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    h = min(base.shape[0], cur.shape[0])
    w = min(base.shape[1], cur.shape[1])
    out = cur[:h, :w].copy()
    out[different] = (255, 0, 0)
    Image.fromarray(out).save(path)


def capture(port, out_dir, only, boot, settle_ms):
    with Device(port, boot=boot, settle_ms=settle_ms) as dev:
        return capture_docs.capture_states(dev, out_dir, only=only)


def main():
    ap = argparse.ArgumentParser(description="Refresh/pixel-check docs screenshots")
    ap.add_argument("--port", default=None)
    ap.add_argument("--update", action="store_true", help="write new baselines")
    ap.add_argument("--only", default=None, help="comma-separated state names")
    ap.add_argument("--no-boot", action="store_true",
                    help="do not reset the board first (it is already up)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the freshly captured PNGs in the temp dir")
    ap.add_argument("--baseline", default=ASSETS,
                    help="baseline directory (default docs/assets)")
    args = ap.parse_args()
    baseline_dir = args.baseline

    port = args.port or os.environ.get("P4_PORT") or "COM3"
    only = args.only.split(",") if args.only else None
    masks = load_masks()

    if args.update:
        names = capture(port, baseline_dir, only, not args.no_boot, 4000)
        print("baseline refreshed: %d frame(s) -> %s" % (len(names), baseline_dir))
        # A refresh is only trustworthy if the very next capture matches it.
        print("re-capturing to verify determinism ...")
        tmp = tempfile.mkdtemp(prefix="shot-verify-")
        names = capture(port, tmp, only, False, 0)
        failed = 0
        for name in names:
            base_path = os.path.join(baseline_dir, name + ".png")
            cur_path = os.path.join(tmp, name + ".png")
            if not (os.path.isfile(base_path) and os.path.isfile(cur_path)):
                print("  %-16s MISSING" % name)
                failed += 1
                continue
            base = load_rgb(base_path)
            cur = load_rgb(cur_path)
            if base.shape != cur.shape:
                print("  %-16s SIZE %s vs %s" % (name, base.shape, cur.shape))
                failed += 1
                continue
            count, bbox, different = diff_frames(base, cur, mask_for(
                name, masks, base.shape[1], base.shape[0]))
            if count:
                failed += 1
                print("  %-16s DIFF %d px %s" % (name, count, bbox))
                write_diff(base, cur, different,
                           os.path.join(DIFFS, name + ".diff.png"))
            else:
                print("  %-16s ok" % name)
        print("determinism: %s" % ("OK" if not failed else "FAILED"))
        return 0 if not failed else 1

    tmp = tempfile.mkdtemp(prefix="shot-check-")
    names = capture(port, tmp, only, not args.no_boot, 4000)
    failed = 0
    for name in names:
        base_path = os.path.join(baseline_dir, name + ".png")
        cur_path = os.path.join(tmp, name + ".png")
        if not os.path.isfile(base_path):
            print("  %-16s no baseline (run --update)" % name)
            failed += 1
            continue
        if not os.path.isfile(cur_path):
            print("  %-16s CAPTURE MISSING" % name)
            failed += 1
            continue
        base = load_rgb(base_path)
        cur = load_rgb(cur_path)
        if base.shape != cur.shape:
            print("  %-16s SIZE %s vs %s" % (name, base.shape, cur.shape))
            failed += 1
            continue
        count, bbox, different = diff_frames(base, cur, mask_for(
            name, masks, base.shape[1], base.shape[0]))
        if count:
            failed += 1
            print("  %-16s DIFF %d px bbox=%s" % (name, count, bbox))
            write_diff(base, cur, different,
                       os.path.join(DIFFS, name + ".diff.png"))
        else:
            print("  %-16s ok" % name)

    if args.keep:
        print("fresh frames kept in %s" % tmp)
    print("SCREENSHOT GATE %s (%d frame(s))"
          % ("OK" if not failed else "FAIL", len(names)))
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
