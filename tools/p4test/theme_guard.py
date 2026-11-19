# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Guard against unstyled LVGL default-theme widgets in a capture.

LVGL's default theme paints any widget the project forgets to style with its
primary blue (``#2196F3``) and its pressed/darker variants. That colour is
never part of this UI: the DOS palette's bright blue is ``#3B78FF`` and every
theme surface is a dark green/amber/grey. A sized blob of the default primary
therefore means an unstyled widget - exactly the bug that shipped the shell's
input-row buttons as a bright blue band (a visible flash every time an app or
modal closed and the row reappeared).

The scan is a sampled colour match, so it is cheap enough to run on every
gallery capture, every dogfood screenshot and over the committed assets.
"""

DEFAULT_THEME_COLORS = {
    "primary": (0x21, 0x96, 0xF3),
    "primary_dark": (0x19, 0x76, 0xD2),
    "primary_darker": (0x0D, 0x47, 0xA1),
}

#: A widget-sized blob, not an anti-aliasing fringe or a web page pixel.
MIN_PIXELS = 120


def scan_image(img, tol=6, min_pixels=MIN_PIXELS, stride=2):
    """Return ``{name: {"count", "bbox"}}`` for sized default-theme blobs.

    @p img is a PIL image. @p stride samples every Nth pixel in both axes; the
    reported count is the raw sampled count (the threshold is tuned for the
    default stride).
    """
    w, h = img.size
    px = img.load()
    counts = {name: 0 for name in DEFAULT_THEME_COLORS}
    boxes = {}

    for y in range(0, h, stride):
        for x in range(0, w, stride):
            c = px[x, y]
            for name, t in DEFAULT_THEME_COLORS.items():
                if (abs(c[0] - t[0]) <= tol and
                        abs(c[1] - t[1]) <= tol and
                        abs(c[2] - t[2]) <= tol):
                    counts[name] += 1
                    b = boxes.get(name)
                    if b is None:
                        boxes[name] = [x, y, x, y]
                    else:
                        if x < b[0]:
                            b[0] = x
                        if y < b[1]:
                            b[1] = y
                        if x > b[2]:
                            b[2] = x
                        if y > b[3]:
                            b[3] = y
                    break

    hits = {}
    for name, count in counts.items():
        if count >= min_pixels:
            hits[name] = {"count": count, "bbox": tuple(boxes[name])}
    return hits


def scan_path(path, **kwargs):
    """Open @p path with PIL and scan it."""
    from PIL import Image

    with Image.open(path) as img:
        return scan_image(img.convert("RGB"), **kwargs)


def describe(hits):
    """One-line, human-readable summary of scan_image()/scan_path() hits."""
    parts = []
    for name, info in sorted(hits.items()):
        x0, y0, x1, y1 = info["bbox"]
        parts.append("%s=%d px at %d,%d-%d,%d"
                     % (name, info["count"], x0, y0, x1, y1))
    return "; ".join(parts)


def scan_bmp(bmp, tol=6, min_pixels=MIN_PIXELS, stride=2):
    """Same scan for a p4test.screenshot.Bmp without building a PIL image."""
    counts = {name: 0 for name in DEFAULT_THEME_COLORS}
    boxes = {}

    for y in range(0, bmp.height, stride):
        for x in range(0, bmp.width, stride):
            c = bmp.px(x, y)
            for name, t in DEFAULT_THEME_COLORS.items():
                if (abs(c[0] - t[0]) <= tol and
                        abs(c[1] - t[1]) <= tol and
                        abs(c[2] - t[2]) <= tol):
                    counts[name] += 1
                    b = boxes.get(name)
                    if b is None:
                        boxes[name] = [x, y, x, y]
                    else:
                        if x < b[0]:
                            b[0] = x
                        if y < b[1]:
                            b[1] = y
                        if x > b[2]:
                            b[2] = x
                        if y > b[3]:
                            b[3] = y
                    break

    hits = {}
    for name, count in counts.items():
        if count >= min_pixels:
            hits[name] = {"count": count, "bbox": tuple(boxes[name])}
    return hits
