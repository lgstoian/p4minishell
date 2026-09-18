#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gen_tui_font.py - generate components/tui/tui_fonts.c + tui_fonts.h.

The TUI is an 80x25 DOS-style grid. Its pixel rect follows the transcript
region (rotation, keyboard), so the cell must be sized from that region, not
fixed. This generator emits extended-unscii cell fonts (ASCII + box drawing +
blocks/symbols) that the TUI picks from:

  * 12x20  - the primary tall cell for the full 1024x510 transcript region
             (80*12 = 960 px wide, 25*20 = 500 px tall).
  * 8x8    - the square fallback for small regions.

A tall cell (height > width) is required for continuous vertical box borders:
with a square glyph plus line spacing the 1-cell vertical strokes break into a
dashed line. Glyphs are rendered from the public-domain unscii-8 TTF at an
intermediate size and resampled into the cell, so text stays legible and the
box drawing spans the full cell.

The generated files are committed; re-run only if the cells change.
Usage: python tools/gen_tui_font.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "scripts",
                   "built_in_font", "unscii-8.ttf")
OUT_C = os.path.join(ROOT, "components", "tui", "tui_fonts.c")
OUT_H = os.path.join(ROOT, "components", "tui", "tui_fonts.h")

# (identifier, cell width, cell height). Order matters: the TUI picks the
# largest cell that fits, preferring width then height.
CELLS = [
    ("12x24", 12, 24),
    ("12x20", 12, 20),
    ("10x16", 10, 16),
    ("8x12", 8, 12),
    ("8x8", 8, 8),
]

# Contiguous ranges (start, length): ASCII, box drawing, misc symbols/blocks.
RANGES = [
    (0x20, 0x60),
    (0x2500, 0x80),
    (0x2600, 0x100),
]


def render_cell(font, code, w, h):
    """Render one glyph into a w x h 1-bit bitmap (PIL), glyph scaled to cell."""
    src = max(w, int(h * 0.8))
    f = ImageFont.truetype(TTF, src)
    im = Image.new("L", (src, src), 0)
    ImageDraw.Draw(im).text((0, 0), chr(code), fill=255, font=f)
    if (src, src) != (w, h):
        im = im.resize((w, h), Image.LANCZOS)
    return im


def pack_bits(im, w, h):
    """Pack a grayscale bitmap into a contiguous MSB-first 1-bit array."""
    out = bytearray()
    acc = 0
    nbits = 0
    for y in range(h):
        for x in range(w):
            acc = (acc << 1) | (1 if im.getpixel((x, y)) > 127 else 0)
            nbits += 1
            if nbits == 8:
                out.append(acc)
                acc = 0
                nbits = 0
    if nbits:
        out.append(acc << (8 - nbits))
    return out


def emit_font(lines, ident, w, h):
    font = ImageFont.truetype(TTF, max(w, int(h * 0.8)))
    glyphs = []
    data = bytearray()
    offsets = []

    # glyph 0 reserved blank (one cell worth of zeros)
    offsets.append(0)
    data.extend(bytes(len(pack_bits(Image.new("L", (w, h), 0), w, h))))
    for start, length in RANGES:
        for code in range(start, start + length):
            offsets.append(len(data))
            data.extend(pack_bits(render_cell(font, code, w, h), w, h))

    lines.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t bitmap_%s[] = {" % ident)
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")

    lines.append("static const lv_font_fmt_txt_glyph_dsc_t dsc_%s[] = {" % ident)
    for i, off in enumerate(offsets):
        if i == 0:
            lines.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, "
                         ".box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
        else:
            lines.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, "
                         ".box_h = %d, .ofs_x = 0, .ofs_y = 0},"
                         % (off, w * 16, w, h))
    lines.append("};")
    lines.append("")

    lines.append("static const lv_font_fmt_txt_cmap_t cmaps_%s[] = {" % ident)
    gid = 1
    for start, length in RANGES:
        lines.append("    {.range_start = 0x%x, .range_length = %d, "
                     ".glyph_id_start = %d, .unicode_list = NULL, "
                     ".glyph_id_ofs_list = NULL, .list_length = 0, "
                     ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}," % (start, length, gid))
        gid += length
    lines.append("};")
    lines.append("")

    lines.append("static const lv_font_fmt_txt_dsc_t fdsc_%s = {" % ident)
    lines.append("    .glyph_bitmap = bitmap_%s," % ident)
    lines.append("    .glyph_dsc = dsc_%s," % ident)
    lines.append("    .cmaps = cmaps_%s," % ident)
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append("    .cmap_num = %d," % len(RANGES))
    lines.append("    .bpp = 1,")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("};")
    lines.append("")
    lines.append("const lv_font_t lv_font_tui_%s = {" % ident)
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append("    .line_height = %d," % h)
    lines.append("    .base_line = 0,")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = 0,")
    lines.append("    .underline_thickness = 0,")
    lines.append("    .dsc = &fdsc_%s," % ident)
    lines.append("};")
    lines.append("")


def main():
    lines = []
    lines.append("/*")
    lines.append(" * SPDX-FileCopyrightText: 2026 Stoian Alexandru")
    lines.append(" * SPDX-License-Identifier: MIT")
    lines.append(" *")
    lines.append(" * Generated by tools/gen_tui_font.py - do not edit by hand.")
    lines.append(" * Extended unscii (ASCII 0x20-0x7F, box 0x2500-0x257F,")
    lines.append(" * symbols 0x2600-0x26FF) rendered into the listed cell sizes.")
    lines.append(" */")
    lines.append("")
    lines.append('#include "tui_fonts.h"')
    lines.append("")
    for ident, w, h in CELLS:
        emit_font(lines, ident, w, h)
    lines.append("const tui_font_cell_t tui_font_cells[] = {")
    for ident, w, h in CELLS:
        lines.append("    { .cell_w = %d, .cell_h = %d, .font = &lv_font_tui_%s },"
                     % (w, h, ident))
    lines.append("};")
    lines.append("const int tui_font_cell_count = (int)(sizeof(tui_font_cells) / "
                 "sizeof(tui_font_cells[0]));")
    lines.append("")

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    header = """/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Generated by tools/gen_tui_font.py - do not edit by hand.
 */
#ifndef P4MINISHELL_TUI_FONTS_H
#define P4MINISHELL_TUI_FONTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One candidate TUI cell: the font plus its logical cell size. */
typedef struct {
    int cell_w;
    int cell_h;
    const lv_font_t *font;
} tui_font_cell_t;

extern const tui_font_cell_t tui_font_cells[];

extern const int tui_font_cell_count;

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_TUI_FONTS_H */
"""
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print("wrote %s and %s (%d cells)" % (OUT_C, OUT_H, len(CELLS)))


if __name__ == "__main__":
    main()
