#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
make_gfx_proof.py - deterministic HOST proof image for the Gfx Toolkit gallery entry.

PROVENANCE - READ THIS BEFORE QUOTING THE IMAGE
-----------------------------------------------
The PNG this tool writes is **not a device capture**. No board was attached when
it was made. It is a host-rendered proof of what the *fixed* ``GFXTOOL.BAT``
draws, so the docs gallery can show the corrected screen while a hardware
recapture is pending.

Nothing in the raster path is reimplemented in Python. The tool compiles the
**real, unmodified** raster core at run time:

  * ``components/gfx/gfx.c``
  * ``components/gfx/gfx_font.c``

...with a tiny C harness (embedded below) that replays the GFXTOOL.BAT
primitive calls in source order, including the BMP decode / rotate / blit of
BALL.BMP and the scaled blit of PHOTO.BMP. The only shims are three one-line
host headers: the ESP PSRAM allocator is ``malloc``/``realloc``/``free`` and
``P4_CONFIG_GFX_FONT_EXTENDED`` is 1 (the shipped default). Sprite and photo
inputs are regenerated with the deterministic PIL recipes documented in
``apps/push_assets.py`` (transcribed here; never read from a board).

The 320x200 RGB565 canvas produced by the real core is expanded to RGB888 with
the same 5/6/5 bit-replication the panel uses, scaled 2x, and pasted onto the
1024x600 gallery panel template (geometry measured from the device captures).
The template chrome is synthetic constants, not pixels copied from an older
screenshot. PIL is used for container/panel work only; no p4test code is copied.

Usage
-----
    python tools/make_gfx_proof.py --check      # committed PNG == deterministic render?
    python tools/make_gfx_proof.py --update     # (re)write the PNG
    python tools/make_gfx_proof.py --self-test  # red canary: prove --check can fail

A C compiler (gcc or cc) must be on PATH. The tool refuses to guess: if the
core cannot be compiled it exits non-zero rather than emitting an imitation.
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG_PATH = os.path.join(ROOT, "docs", "assets", "apps", "gfxtool.png")
GFX_SRC = os.path.join(ROOT, "components", "gfx")

CANVAS_W, CANVAS_H = 320, 200
PANEL_W, PANEL_H = 1024, 600
SCALE = 2
ORIGIN_X, ORIGIN_Y = 192, 76          # panel position of the 640x400 canvas
CHROME_RGB = (16, 12, 0)              # measured shell side strips / bottom bar
CHROME_SIDE = 8
CHROME_BAR_TOP = 552

# Source colours from GFXTOOL.BAT (RGB888 -> RGB565 is done by the real core).
CLEAR = 0x101820
BORDER = 0x3A6EA5
TRIANGLE = 0xFF5555
ELLIPSE = 0x55FF55
POLYGON = 0x5599FF
ORANGE = 0xFFAA00
WHITE = 0xFFFFFF
YELLOW = 0xFFFF00
CYAN = 0x00FFFF
RED_CORE = 0xFF0000

# Text rectangles in CANVAS coordinates, derived from gfx_surface_text's
# advance of GFX_FONT_W*scale per character (8x8 font).
TITLE = dict(x=18, y=150, scale=2, text="Gfx Toolkit", color=WHITE)
LEGEND = dict(x=18, y=170, scale=1, text="tri ellipse poly fill text", color=CYAN)


# --------------------------------------------------------------------------
# Host shims and harness. The gfx sources themselves are compiled verbatim.
# --------------------------------------------------------------------------
STUB_HEADERS = {
    "esp_heap_caps.h": (
        "#pragma once\n#include <stddef.h>\nvoid heap_caps_free(void *p);\n"
    ),
    "p4heap.h": (
        "#pragma once\n#include <stddef.h>\n"
        "void *p4heap_alloc_psram(size_t n);\n"
        "void *p4heap_realloc_psram(void *p, size_t n);\n"
    ),
    "p4minishell_config.h": (
        "#pragma once\n#define P4_CONFIG_GFX_FONT_EXTENDED 1\n"
    ),
}

HARNESS_C = r"""
/* Host harness: replay GFXTOOL.BAT against the real gfx raster core.
 * argv: <ball.bmp> <photo.bmp> <out.raw> <stale:0|1>
 * stale=1 draws the text BEFORE the sprite stamps (the v1.4.3 bug), used by
 * the red-canary self-test only; --update/--check always use stale=0.
 */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>

void heap_caps_free(void *p) { free(p); }
void *p4heap_alloc_psram(size_t n) { return malloc(n); }
void *p4heap_realloc_psram(void *p, size_t n) { return realloc(p, n); }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;

    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)n);
    if (buf == NULL) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void stamp_balls(gfx_surface_t *s, const gfx_surface_t *ball)
{
    int sx = 24;
    int sy = 160;
    int k;

    for (k = 0; k < 4; k++) {            /* /r:0 /r:90 /r:180 /r:270 */
        gfx_surface_t rot;
        if (!gfx_surface_rotate_cw(ball, k, &rot)) exit(4);
        gfx_surface_blit(s, &rot, sx, sy, true, gfx_rgb_to_565(0x000000));
        gfx_surface_free(&rot);
        sx += 34;
        sy -= 14;
    }
}

static void draw_text(gfx_surface_t *s)
{
    /* The fix: text is drawn AFTER the sprite stamps (GFXTOOL.BAT lines 33-34). */
    gfx_surface_text(s, 18, 150, "Gfx Toolkit",
                     gfx_rgb_to_565(0xFFFFFF), 0, false, 2);
    gfx_surface_text(s, 18, 170, "tri ellipse poly fill text",
                     gfx_rgb_to_565(0x00FFFF), 0, false, 1);
}

int main(int argc, char **argv)
{
    gfx_surface_t s;
    gfx_surface_t ball = {NULL, 0, 0};
    gfx_surface_t img = {NULL, 0, 0};
    gfx_bmp_info_t info;
    uint8_t *ball_buf, *photo_buf;
    size_t ball_n = 0, photo_n = 0;
    int stale;
    FILE *out;
    int poly[6] = {215, 150, 300, 150, 257, 40};

    if (argc != 5) return 9;
    stale = atoi(argv[4]);

    if (!gfx_surface_alloc(&s, 320, 200)) return 1;

    /* GFXTOOL.BAT lines 16-24, in source order. */
    gfx_surface_clear(&s, gfx_rgb_to_565(0x101820));
    gfx_surface_rect(&s, 4, 4, 312, 192, gfx_rgb_to_565(0x3A6EA5), false);
    gfx_surface_triangle(&s, 20, 30, 140, 30, 20, 150,
                         gfx_rgb_to_565(0xFF5555), true);
    gfx_surface_ellipse(&s, 185, 95, 65, 50, gfx_rgb_to_565(0x55FF55), true);
    gfx_surface_polygon(&s, poly, 3, gfx_rgb_to_565(0x5599FF), true);
    gfx_surface_rect(&s, 230, 12, 72, 40, gfx_rgb_to_565(0xFFFFFF), false);
    gfx_surface_flood_fill(&s, 266, 30, gfx_rgb_to_565(0xFFAA00));
    gfx_surface_hline(&s, 8, 190, 304, gfx_rgb_to_565(0xFFFFFF));
    gfx_surface_vline(&s, 158, 20, 120, gfx_rgb_to_565(0xFFFF00));

    /* gfx load 0 BALL.BMP + gfx blitmany stamps. */
    ball_buf = slurp(argv[1], &ball_n);
    if (ball_buf == NULL || !gfx_bmp_parse_header(ball_buf, ball_n, &info)) return 2;
    if (!gfx_bmp_decode_565(ball_buf, ball_n, &info, &ball)) return 3;
    free(ball_buf);

    if (stale) {
        draw_text(&s);
        stamp_balls(&s, &ball);
    } else {
        stamp_balls(&s, &ball);
        draw_text(&s);
    }
    gfx_surface_free(&ball);

    /* gfx image PHOTO.BMP 232 154 80 32 (last, as in the .BAT). */
    photo_buf = slurp(argv[2], &photo_n);
    if (photo_buf == NULL) return 5;
    if (!gfx_bmp_parse_header_ex(photo_buf, photo_n, &info,
                                 GFX_IMAGE_MAX_W, GFX_IMAGE_MAX_H)) return 6;
    if (!gfx_bmp_decode_scaled_565(photo_buf, photo_n, &info, 80, 32, &img)) return 7;
    free(photo_buf);
    gfx_surface_blit(&s, &img, 232, 154, false, 0);
    gfx_surface_free(&img);

    out = fopen(argv[3], "wb");
    if (out == NULL) return 8;
    if (fwrite(s.px, 2, (size_t)s.w * (size_t)s.h, out) != (size_t)s.w * (size_t)s.h) {
        fclose(out); return 8;
    }
    fclose(out);
    gfx_surface_free(&s);
    return 0;
}
"""


# --------------------------------------------------------------------------
# Deterministic inputs (transcribed from apps/push_assets.py).
# --------------------------------------------------------------------------
def make_ball(path: str) -> None:
    """16x16 BALL: white disc with red core on black (transparency demo)."""
    im = Image.new("RGB", (16, 16), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([1, 1, 14, 14], fill=(255, 255, 255))
    d.ellipse([5, 5, 10, 10], fill=(255, 0, 0))
    im.save(path)


def make_photo(path: str) -> None:
    """96x64 PHOTO.BMP: gradient sky + sun + hills for the `gfx image` blit."""
    im = Image.new("RGB", (96, 64))
    px = im.load()
    for y in range(64):
        for x in range(96):
            px[x, y] = (int(20 + (y / 63.0) * 200),
                        int(60 + (y / 63.0) * 120),
                        int(160 - (y / 63.0) * 120))
    d = ImageDraw.Draw(im)
    d.ellipse([64, 8, 84, 28], fill=(255, 230, 120))
    d.polygon([(0, 64), (30, 30), (60, 64)], fill=(30, 110, 45))
    d.polygon([(40, 64), (72, 36), (96, 64)], fill=(20, 80, 35))
    im.save(path)


# --------------------------------------------------------------------------
# Real-core render.
# --------------------------------------------------------------------------
def _compiler() -> str:
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        raise RuntimeError("no C compiler (gcc/cc) on PATH; refusing to fake the gfx core")
    return cc


def build_harness(tmp: str) -> str:
    for name, body in STUB_HEADERS.items():
        with open(os.path.join(tmp, name), "w", encoding="utf-8") as fh:
            fh.write(body)
    with open(os.path.join(tmp, "harness.c"), "w", encoding="utf-8") as fh:
        fh.write(HARNESS_C)
    make_ball(os.path.join(tmp, "BALL.BMP"))
    make_photo(os.path.join(tmp, "PHOTO.BMP"))

    exe = os.path.join(tmp, "harness.exe" if os.name == "nt" else "harness")
    cmd = [_compiler(), "-std=c11", "-O2", "-I", tmp, "-I", GFX_SRC,
           os.path.join(tmp, "harness.c"),
           os.path.join(GFX_SRC, "gfx.c"), os.path.join(GFX_SRC, "gfx_font.c"),
           "-lm", "-o", exe]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("gfx core compile failed:\n%s\n%s" % (proc.stdout, proc.stderr))
    return exe


def render_canvas(exe: str, tmp: str, stale: bool = False) -> list[int]:
    """Return the 320x200 RGB565 canvas from the real core.

    ``stale=True`` is used only by the self-test to reconstruct the buggy
    text-before-sprites draw order.
    """
    raw = os.path.join(tmp, "stale.raw" if stale else "proof.raw")
    cmd = [exe, os.path.join(tmp, "BALL.BMP"), os.path.join(tmp, "PHOTO.BMP"),
           raw, "1" if stale else "0"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("gfx harness failed (rc=%d): %s %s"
                           % (proc.returncode, proc.stdout, proc.stderr))
    data = open(raw, "rb").read()
    want = CANVAS_W * CANVAS_H * 2
    if len(data) != want:
        raise RuntimeError("gfx harness wrote %d bytes, expected %d" % (len(data), want))
    return list(struct.unpack("<%dH" % (CANVAS_W * CANVAS_H), data))


# --------------------------------------------------------------------------
# Panel composition and pixel model (PIL container only).
# --------------------------------------------------------------------------
def expand565(value: int) -> tuple[int, int, int]:
    """RGB565 -> RGB888 with full bit replication (the panel's conversion)."""
    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def compose(canvas: list[int]) -> Image.Image:
    """Place the 2x-scaled canvas on the synthetic 1024x600 gallery panel."""
    panel = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    px = panel.load()
    for y in range(PANEL_H):
        if y >= CHROME_BAR_TOP:
            for x in range(PANEL_W):
                px[x, y] = CHROME_RGB
        else:
            for x in range(CHROME_SIDE):
                px[x, y] = CHROME_RGB
            for x in range(PANEL_W - CHROME_SIDE, PANEL_W):
                px[x, y] = CHROME_RGB

    rgb = bytes(v for c in canvas for v in expand565(c))
    small = Image.frombytes("RGB", (CANVAS_W, CANVAS_H), rgb)
    big = small.resize((CANVAS_W * SCALE, CANVAS_H * SCALE), Image.NEAREST)
    panel.paste(big, (ORIGIN_X, ORIGIN_Y))
    return panel


def panel_xy(cx: int, cy: int) -> tuple[int, int]:
    return (ORIGIN_X + cx * SCALE, ORIGIN_Y + cy * SCALE)


def text_rect(spec: dict) -> tuple[int, int, int, int]:
    """Inclusive canvas rect spanned by a gfx_surface_text call."""
    w = len(spec["text"]) * 8 * spec["scale"]
    h = 8 * spec["scale"]
    return (spec["x"], spec["y"], spec["x"] + w - 1, spec["y"] + h - 1)


def ink_pixels(canvas: list[int], spec: dict) -> list[tuple[int, int]]:
    """Canvas pixels holding the glyph ink colour inside the label rect."""
    x0, y0, x1, y1 = text_rect(spec)
    ink = gfx_rgb_to_565_py(spec["color"])
    return [(x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)
            if canvas[y * CANVAS_W + x] == ink]


def gfx_rgb_to_565_py(rgb: int) -> int:
    """Mirror of gfx_rgb_to_565 for building *expected* pixel values only."""
    return (((rgb >> 16) & 0xFF) >> 3) << 11 | (((rgb >> 8) & 0xFF) >> 2) << 5 | ((rgb & 0xFF) >> 3)


def occlusion_set(correct: list[int], stale: list[int], spec: dict) -> list[tuple[int, int]]:
    """Ink pixels a sprite would cover in the buggy draw order.

    These are the exact pixels the v1.4.3 fix changed: with text drawn last the
    glyph ink is on top, in the stale render the sprite wins.
    """
    return [(x, y) for (x, y) in ink_pixels(correct, spec)
            if stale[y * CANVAS_W + x] != correct[y * CANVAS_W + x]]


# --------------------------------------------------------------------------
# Verification.
# --------------------------------------------------------------------------
def verify(img: Image.Image, correct_canvas: list[int], stale_canvas: list[int]) -> dict:
    """Named, independently meaningful checks on a proposed proof image."""
    results: dict[str, bool] = {}
    results["size"] = img.size == (PANEL_W, PANEL_H) and img.mode == "RGB"

    if not results["size"]:
        for key in ("deterministic_match", "colours", "title", "legend"):
            results[key] = False
        return results

    px = img.load()

    # 1. Deterministic identity: every panel pixel equals a fresh real-core render.
    expected = compose(correct_canvas)
    results["deterministic_match"] = img.tobytes() == expected.tobytes()

    # 2. Signatures of all primitive colours are present (quantized by the core).
    x0, y0, x1, y1 = 192, 76, 831, 475
    present = {expand565(c) for c in correct_canvas}
    for colour in (CLEAR, BORDER, TRIANGLE, ELLIPSE, POLYGON, ORANGE,
                   WHITE, YELLOW, CYAN):
        if expand565(gfx_rgb_to_565_py(colour)) not in present:
            results["colours"] = False
            break
    else:
        results["colours"] = True
    # Cross-check against the actual PNG pixels (not just the in-memory canvas).
    if results["colours"]:
        for colour in (BORDER, TRIANGLE, ELLIPSE, POLYGON, ORANGE, CYAN):
            want = expand565(gfx_rgb_to_565_py(colour))
            if not any(px[x, y] == want for y in range(y0, y1 + 1, 2)
                       for x in range(x0, x1 + 1, 2)):
                results["colours"] = False
                break

    # 3/4. The fix itself: ink pixels that sprites used to cover are now the
    # glyph colour, and the last glyph of each label is inside the canvas
    # (i.e. the label is not clipped). Checked on panel pixels via 2x blocks.
    for key, spec in (("title", TITLE), ("legend", LEGEND)):
        occ = occlusion_set(correct_canvas, stale_canvas, spec)
        want = expand565(gfx_rgb_to_565_py(spec["color"]))
        covered = [pt for pt in occ
                   if not all(px[ORIGIN_X + pt[0] * SCALE + dx,
                                ORIGIN_Y + pt[1] * SCALE + dy] == want
                              for dx in range(SCALE) for dy in range(SCALE))]
        ink = ink_pixels(correct_canvas, spec)
        lx = max((p[0] for p in ink), default=-1)
        full_right = lx >= text_rect(spec)[2] - spec["scale"] * 8
        results[key] = bool(occ) and not covered and bool(ink) and full_right
    return results


def report(results: dict) -> None:
    for key in ("size", "deterministic_match", "colours", "title", "legend"):
        print("  [%s] %s" % ("PASS" if results.get(key) else "FAIL", key))


# --------------------------------------------------------------------------
# Modes.
# --------------------------------------------------------------------------
def render_all() -> tuple[Image.Image, list[int], list[int]]:
    with tempfile.TemporaryDirectory(prefix="make_gfx_proof_") as tmp:
        exe = build_harness(tmp)
        correct = render_canvas(exe, tmp, stale=False)
        stale = render_canvas(exe, tmp, stale=True)
    return compose(correct), correct, stale


def self_test() -> int:
    """RED CANARY: a guard that was never seen failing is not a guard."""
    correct_img, correct, stale = render_all()

    # Green: the honest render must satisfy every check.
    good = verify(correct_img, correct, stale)
    green_ok = all(good.values())

    # Red 1: the stale draw order (text first, sprites on top) must be
    # rejected specifically by the title/legend occlusion checks.
    bad_order = verify(compose(stale), correct, stale)
    order_ok = (not bad_order["title"] and not bad_order["legend"]
                and not bad_order["deterministic_match"])

    # Red 2: a clipped/covered title must be rejected even when every other
    # pixel is perfect. This proves the "labels are not clipped" check bites.
    clipped = correct_img.copy()
    cp = clipped.load()
    for y in range(panel_xy(18, 150)[1], panel_xy(18, 150)[1] + 32):
        for x in range(panel_xy(18, 150)[0], panel_xy(18, 150)[0] + 352):
            cp[x, y] = expand565(gfx_rgb_to_565_py(CLEAR))
    bad_clip = verify(clipped, correct, stale)
    clip_ok = not bad_clip["title"] and bad_clip["legend"] and bad_clip["colours"]

    print("self-test green render:", "PASS" if green_ok else "FAIL")
    report(good)
    print("self-test red canary 1 (sprites over text):", "PASS" if order_ok else "FAIL")
    print("self-test red canary 2 (clipped title):", "PASS" if clip_ok else "FAIL")
    ok = green_ok and order_ok and clip_ok
    print("make_gfx_proof self-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    if "--update" in args:
        img, _, _ = render_all()
        os.makedirs(os.path.dirname(PNG_PATH), exist_ok=True)
        img.save(PNG_PATH)
        print("wrote %s (%dx%d, host-generated proof)" % (PNG_PATH, *img.size))
        return 0
    if "--check" in args or not args:
        if not os.path.exists(PNG_PATH):
            print("check: MISSING %s (run --update)" % PNG_PATH)
            return 1
        img = Image.open(PNG_PATH).convert("RGB")
        proof, correct, stale = render_all()
        results = verify(img, correct, stale)
        report(results)
        ok = all(results.values())
        print("check: %s (host-generated proof; not a device capture)" %
              ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
