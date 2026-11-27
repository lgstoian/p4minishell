#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Regenerate the small, deterministic video-gate fixtures.

No images are copied from a project: each BMP is painted here from three
RGB triples, and the USTAR container is written from those same bytes using
the documented 512-byte header layout. Run this only when changing a fixture;
the gate consumes the committed files, never a live generator.
"""
from __future__ import annotations

import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
COLOURS = ((0xD0, 0x20, 0x20), (0x20, 0xD0, 0x20), (0x20, 0x50, 0xD0))
NAMES = ("frame_000.bmp", "frame_001.bmp", "frame_002.bmp")


def bmp(rgb: tuple[int, int, int], width: int = 8, height: int = 8) -> bytes:
    """24-bit bottom-up BMP with 4-byte row padding."""
    row_len = ((width * 3 + 3) // 4) * 4
    pixels = bytearray()
    for _y in range(height):
        row = bytearray()
        for _x in range(width):
            row += bytes((rgb[2], rgb[1], rgb[0]))
        row += bytes(row_len - len(row))
        pixels = row + pixels  # bottom-up rows
    size = 54 + len(pixels)
    header = b"BM" + struct.pack("<IHHI", size, 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                       len(pixels), 2835, 2835, 0, 0)
    return header + info + bytes(pixels)


def octal(value: int, width: int) -> bytes:
    text = ("%0*o" % (width - 1, value))[-width:]
    return text.encode("ascii") + b"\0"


def ustar_header(name: str, size: int, mtime: int = 1700000000,
                 is_dir: bool = False) -> bytes:
    block = bytearray(512)
    block[0:len(name)] = name.encode("ascii")
    block[100:108] = octal(0o755 if is_dir else 0o644, 8)
    block[108:116] = octal(0, 8)
    block[116:124] = octal(0, 8)
    block[124:136] = octal(size, 12)
    block[136:148] = octal(mtime, 12)
    block[148:156] = b"        "  # checksum field counted as spaces
    block[156:157] = b"5" if is_dir else b"0"
    block[257:263] = b"ustar\0"
    block[263:265] = b"00"
    block[265:269] = b"root"
    block[297:301] = b"root"
    checksum = sum(block)
    block[148:154] = ("%06o" % checksum).encode("ascii")  # 6 octal digits
    block[154:155] = b"\0"
    block[155:156] = b" "
    return bytes(block)


def ustar_member(name: str, data: bytes) -> bytes:
    padded = data + bytes((-len(data)) % 512)
    return ustar_header(name, len(data)) + padded


def main() -> None:
    frames = []
    for name, colour in zip(NAMES, COLOURS):
        data = bmp(colour)
        frames.append(data)
        (HERE / name).write_bytes(data)
    (HERE / "MANIFEST.TXT").write_text(
        "# ordered frame list\n" + "\n".join(NAMES) + "\nNOT_AN_IMAGE.TXT\n",
        encoding="utf-8")
    (HERE / "NOT_AN_IMAGE.TXT").write_text("not a frame\n", encoding="utf-8")

    archive = b"".join(ustar_member("FRAMES/" + name, data)
                       for name, data in zip(NAMES, frames))
    archive += bytes(1024)
    (HERE / "frames.p4a").write_bytes(archive)

    # Small EPUB3 nav + EPUB2 NCX documents for the real Expat parsers.
    (HERE / "toc.ncx").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
        '<navMap>'
        '<navPoint id="n1" playOrder="1"><navLabel><text>One</text></navLabel>'
        '<content src="Text/c1.xhtml"/></navPoint>'
        '<navPoint id="n2" playOrder="2"><navLabel><text>Two</text></navLabel>'
        '<content src="Text/c2.xhtml"/></navPoint>'
        '<navPoint id="n3" playOrder="3"><navLabel><text>Missing</text></navLabel>'
        '<content src="Text/c999.xhtml"/></navPoint>'
        '</navMap></ncx>\n', encoding="utf-8")
    (HERE / "nav.xhtml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<html xmlns="http://www.w3.org/1999/xhtml" '
        'xmlns:epub="http://www.idpf.org/2007/ops">'
        '<body><nav epub:type="toc"><ol>'
        '<li><a href="Text/c1.xhtml">One</a></li>'
        '<li><a href="Text/c2.xhtml">Two</a></li>'
        '</ol></nav></body></html>\n', encoding="utf-8")


if __name__ == "__main__":
    main()
