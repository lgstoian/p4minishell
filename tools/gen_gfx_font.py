#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gen_gfx_font.py - generate components/gfx/gfx_font.c (8x8 CP437 font).

Renders full CP437 (0x00..0xFF) from the public-domain unscii-8 TTF
bundled with LVGL into an 8x8 1-bit table for the pure `gfx_surface_text`
rasterizer. The generated C file is committed; re-run only if the font or
cell size changes.

The table below is the IBM CP437 code page, NOT Latin-1: `chr(code)` is
only correct for 0x00-0x7F. Mapping 0x80-0xFF with `chr()` would produce
Latin-1, which renders CP437 0x80-0x9F (C, u, e, a, ...) and the whole
0xB0-0xDF box-drawing/block range as blank or as the wrong glyph. The
explicit table is the only correct source for the upper half.

Usage: python tools/gen_gfx_font.py
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "scripts",
                   "built_in_font", "unscii-8.ttf")
OUT = os.path.join(ROOT, "components", "gfx", "gfx_font.c")

FIRST = 0x00
LAST = 0xFF
W = 8
H = 8

# IBM CP437 code page, index == byte value. None means "no glyph": the
# C0 controls (0x00-0x1F) and 0x7F are consumed by the firmware as
# control/whitespace and are intentionally left blank.
CP437 = [None] * 256

_ASCII_PRINTABLE = {
    0x20: " ", 0x21: "!", 0x22: '"', 0x23: "#", 0x24: "$", 0x25: "%",
    0x26: "&", 0x27: "'", 0x28: "(", 0x29: ")", 0x2A: "*", 0x2B: "+",
    0x2C: ",", 0x2D: "-", 0x2E: ".", 0x2F: "/", 0x3A: ":", 0x3B: ";",
    0x3C: "<", 0x3D: "=", 0x3E: ">", 0x3F: "?", 0x40: "@", 0x5B: "[",
    0x5C: "\\", 0x5D: "]", 0x5E: "^", 0x5F: "_", 0x60: "`", 0x7B: "{",
    0x7C: "|", 0x7D: "}", 0x7E: "~",
}
for _code in range(0x20, 0x7F):
    CP437[_code] = _code

# 0x80-0x9F: letters and symbols. Latin-1 is WRONG here (those codepoints
# are C1 controls in Latin-1).
CP437[0x80:0xA0] = [
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,  # 80-87
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,  # 88-8F
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,  # 90-97
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,  # 98-9F
]

# 0xA0-0xAF: accented letters and punctuation (matches Latin-1 here).
CP437[0xA0:0xB0] = [
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,  # A0-A7
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,  # A8-AF
]

# 0xB0-0xDF: box drawing, blocks, and geometric shapes.
CP437[0xB0:0xE0] = [
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,  # B0-B7
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,  # B8-BF
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,  # C0-C7
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,  # C8-CF
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,  # D0-D7
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,  # D8-DF
]

# 0xE0-0xFF: Greek, math, and the tail punctuation.
CP437[0xE0:0x100] = [
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,  # E0-E7
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,  # E8-EF
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,  # F0-F7
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,  # F8-FF
]

# unscii-8 has no glyph for these two CP437 cells (verified against the
# font cmap). Rendering them blank would leave two visible holes in the
# character set, so each is drawn from the nearest glyph the font DOES
# carry. CP437 0xA9 is a reversed-not sign and the font's U+00AC (not
# sign) is the same shape; CP437 0xF9 is a bullet operator and U+2022
# (bullet) is the closest match while staying distinct from 0xFA's U+00B7
# middle dot.
SUBSTITUTIONS = {
    0x2310: 0x00AC,  # CP437 0xA9 reversed-not -> not sign
    0x2219: 0x2022,  # CP437 0xF9 bullet operator -> bullet
}


def render(font):
    glyphs = []
    blanks = []
    for code in range(FIRST, LAST + 1):
        im = Image.new("L", (W, H), 0)
        cp = CP437[code]
        if cp is not None:
            draw_cp = SUBSTITUTIONS.get(cp, cp)
            try:
                ImageDraw.Draw(im).text((0, 0), chr(draw_cp), fill=255, font=font)
            except (ValueError, OverflowError):
                # Glyph missing from the TTF - leave blank rather than abort.
                pass
        rows = []
        for y in range(H):
            bits = 0
            for x in range(W):
                if im.getpixel((x, y)) > 127:
                    bits |= 1 << (7 - x)
            rows.append(bits)
        if code != FIRST and cp is not None and all(b == 0 for b in rows):
            blanks.append(code)
        glyphs.append((code, cp, rows))
    return glyphs, blanks


def emit(glyphs):
    lines = [
        "/**",
        " * @file gfx_font.c",
        " * @brief Generated 8x8 CP437 glyph table for `gfx_surface_text`.",
        " *",
        " * Generated by tools/gen_gfx_font.py from the public-domain unscii-8",
        " * TTF (bundled with LVGL scripts/built_in_font). Do not edit by hand.",
        " * Rows are MSB-first (bit 7 = leftmost pixel), one byte per row.",
        " * Covers full CP437 range 0x00-0xFF (256 glyphs).",
        " */",
        "",
        '#include "gfx.h"',
        "",
        "const uint8_t gfx_font8x8[GFX_FONT_GLYPHS][GFX_FONT_H] = {",
    ]
    for code, cp, rows in glyphs:
        body = ", ".join("0x%02X" % b for b in rows)
        if cp is None:
            label = "control"
        elif 0x20 <= code <= 0x7E:
            label = _ASCII_PRINTABLE.get(code, chr(code))
        else:
            label = "U+%04X" % cp
        lines.append("    { %s }, /* 0x%02X '%s' */" % (body, code, label))
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    if not os.path.exists(TTF):
        print("FAIL: font not found: %s" % TTF)
        return 1
    font = ImageFont.truetype(TTF, H)
    glyphs, blanks = render(font)
    if len(glyphs) != LAST - FIRST + 1:
        print("FAIL: expected %d glyphs, got %d" % (LAST - FIRST + 1, len(glyphs)))
        return 1
    with open(OUT, "w", encoding="ascii", newline="\n") as f:
        f.write(emit(glyphs))
    print("WROTE %s (%d glyphs, %d bytes)" % (OUT, len(glyphs),
                                              os.path.getsize(OUT)))
    if blanks:
        print("WARN blank glyphs: %s" % " ".join("0x%02X" % c for c in blanks))
    else:
        print("OK no unexpected blank glyphs")
    return 0


if __name__ == "__main__":
    sys.exit(main())