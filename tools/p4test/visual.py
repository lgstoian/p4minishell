# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Pixel and geometry analytics for the visual-accuracy sweep.

This is test-side only: it turns a :class:`~p4test.screenshot.Bmp` (and the
firmware's `ui targets` geometry dump) into concrete, assertable numbers so
visual defects (clipping, off-screen widgets, overlaps, misaligned rows,
broken borders, stale colours) can be detected without golden images.

Nothing here touches the firmware.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

from .screenshot import Bmp

Rect = Tuple[int, int, int, int]   # x, y, w, h


# --------------------------------------------------------------------------
# Pixel model
# --------------------------------------------------------------------------

def array(bmp: Bmp) -> np.ndarray:
    """Return the frame as an (H, W, 3) uint8 RGB array, origin top-left."""
    count = bmp.stride * bmp.height
    raw = np.frombuffer(bmp.data, dtype=np.uint8, count=count,
                        offset=bmp.data_offset)
    rows = raw.reshape(bmp.height, bmp.stride)
    pix = rows[:, :bmp.width * 3].reshape(bmp.height, bmp.width, 3)
    rgb = pix[:, :, ::-1]                      # BMP is BGR
    if not bmp.top_down:
        rgb = rgb[::-1, :, :]
    return np.ascontiguousarray(rgb)


def bg_color(arr: np.ndarray) -> Tuple[int, int, int]:
    """Modal background colour (the most common pixel)."""
    flat = arr.reshape(-1, 3)
    # Sample every 3rd pixel for speed; backgrounds dominate.
    sample = flat[::3]
    colors, counts = np.unique(sample, axis=0, return_counts=True)
    return tuple(int(v) for v in colors[int(np.argmax(counts))])


def nonbg_mask(arr: np.ndarray, bg: Optional[Tuple[int, int, int]] = None,
               tol: int = 24) -> np.ndarray:
    """Boolean mask of pixels that differ from the background by > tol."""
    if bg is None:
        bg = bg_color(arr)
    diff = np.abs(arr.astype(np.int16) - np.array(bg, dtype=np.int16))
    return (diff.max(axis=2) > tol)


def rect_bbox(mask: np.ndarray, rect: Optional[Rect] = None) -> Optional[Rect]:
    """Bounding box of set pixels inside ``rect`` (x, y, w, h) or the whole mask."""
    if rect is not None:
        x, y, w, h = rect
        sub = mask[max(0, y):y + h, max(0, x):x + w]
        ox, oy = max(0, x), max(0, y)
    else:
        sub = mask
        ox, oy = 0, 0
    if not sub.any():
        return None
    ys, xs = np.where(sub)
    return (int(xs.min()) + ox, int(ys.min()) + oy,
            int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1))


def row_runs(mask: np.ndarray, min_pixels: int = 1) -> List[Tuple[int, int]]:
    """Contiguous horizontal bands (y0, y1-exclusive) that contain ink."""
    rows = mask.sum(axis=1) >= min_pixels
    runs: List[Tuple[int, int]] = []
    start = None
    for i, on in enumerate(rows):
        if on and start is None:
            start = i
        elif not on and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(rows)))
    return runs


def col_runs(mask: np.ndarray, min_pixels: int = 1) -> List[Tuple[int, int]]:
    cols = mask.sum(axis=0) >= min_pixels
    runs: List[Tuple[int, int]] = []
    start = None
    for i, on in enumerate(cols):
        if on and start is None:
            start = i
        elif not on and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(cols)))
    return runs


def edge_ink(mask: np.ndarray, margin: int = 2) -> Dict[str, int]:
    """Count ink pixels within ``margin`` of each screen edge."""
    h, w = mask.shape
    m = max(1, margin)
    return {
        "top": int(mask[:m, :].sum()),
        "bottom": int(mask[h - m:, :].sum()),
        "left": int(mask[:, :m].sum()),
        "right": int(mask[:, w - m:].sum()),
    }


def color_fraction(arr: np.ndarray, rect: Rect, rgb: Tuple[int, int, int],
                   tol: int = 24) -> float:
    """Fraction of pixels in ``rect`` within ``tol`` of ``rgb``."""
    x, y, w, h = rect
    sub = arr[max(0, y):y + h, max(0, x):x + w]
    if sub.size == 0:
        return 0.0
    d = np.abs(sub.astype(np.int16) - np.array(rgb, dtype=np.int16))
    return float((d.max(axis=2) <= tol).mean())


def row_gaps(mask: np.ndarray, y: int) -> List[Tuple[int, int]]:
    """Gap intervals (x0, x1-exclusive) on row ``y`` with no ink."""
    if y < 0 or y >= mask.shape[0]:
        return []
    line = mask[y]
    gaps: List[Tuple[int, int]] = []
    start = None
    for i, on in enumerate(line):
        if not on and start is None:
            start = i
        elif on and start is not None:
            gaps.append((start, i))
            start = None
    if start is not None:
        gaps.append((start, len(line)))
    return gaps


def col_gaps(mask: np.ndarray, x: int) -> List[Tuple[int, int]]:
    if x < 0 or x >= mask.shape[1]:
        return []
    line = mask[:, x]
    gaps: List[Tuple[int, int]] = []
    start = None
    for i, on in enumerate(line):
        if not on and start is None:
            start = i
        elif on and start is not None:
            gaps.append((start, i))
            start = None
    if start is not None:
        gaps.append((start, len(line)))
    return gaps


# --------------------------------------------------------------------------
# Geometry model (from the firmware `ui targets` dump)
# --------------------------------------------------------------------------

@dataclass
class Target:
    id: int
    x: int
    y: int
    w: int
    h: int
    name: str
    clipped: bool = False

    @property
    def rect(self) -> Rect:
        return (self.x, self.y, self.w, self.h)

    @property
    def is_kbd(self) -> bool:
        return self.name.startswith("kbd:")

    @property
    def kbd_label(self) -> str:
        return self.name[4:] if self.is_kbd else ""


def parse_targets(text: str) -> List[Target]:
    """Parse `ui targets` output. The name is printed last (may contain spaces
    for list rows, may be empty, may contain PUA glyphs). A trailing ``[C]``
    marks a clipped (not visible/tappable) target, e.g. an off-view scroll-list
    row; it is stripped from the name and exposed as ``Target.clipped``."""
    out: List[Target] = []
    for line in text.splitlines():
        m = re.match(r"^\s*(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)(?:\s+(.*))?$",
                     line)
        if not m:
            continue
        name = (m.group(6) or "").strip()
        clipped = name.endswith("[C]")
        if clipped:
            name = name[:-3].rstrip()
        out.append(Target(int(m.group(1)), int(m.group(2)), int(m.group(3)),
                          int(m.group(4)), int(m.group(5)), name, clipped))
    return out


def rect_area(r: Rect) -> int:
    return max(0, r[2]) * max(0, r[3])


def intersection(a: Rect, b: Rect) -> int:
    ax0, ay0, aw, ah = a
    bx0, by0, bw, bh = b
    ix = max(0, min(ax0 + aw, bx0 + bw) - max(ax0, bx0))
    iy = max(0, min(ay0 + ah, by0 + bh) - max(ay0, by0))
    return ix * iy


def overlap_ratio(a: Rect, b: Rect) -> float:
    inter = intersection(a, b)
    if inter == 0:
        return 0.0
    smaller = max(1, min(rect_area(a), rect_area(b)))
    return inter / smaller


def within(inner: Rect, outer: Rect, slack: int = 0) -> bool:
    ix, iy, iw, ih = inner
    ox, oy, ow, oh = outer
    return (ix >= ox - slack and iy >= oy - slack and
            ix + iw <= ox + ow + slack and iy + ih <= oy + oh + slack)


def group_rows(targets: Sequence[Target], tol: int = 6) -> List[List[Target]]:
    """Group targets whose y-centres are within ``tol`` (visual rows)."""
    ordered = sorted(targets, key=lambda t: (t.y + t.h / 2.0, t.x))
    groups: List[List[Target]] = []
    for t in ordered:
        cy = t.y + t.h / 2.0
        if groups:
            ref = groups[-1][0]
            if abs((ref.y + ref.h / 2.0) - cy) <= tol:
                groups[-1].append(t)
                continue
        groups.append([t])
    return groups
