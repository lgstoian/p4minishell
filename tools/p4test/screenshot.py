# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Streaming-screenshot capture and pixel verification.

The firmware ``screenshot`` command (no argument) suspends the console reader
and streams ``BMPX`` + LE32 size + raw 24-bit BMP over USB-Serial-JTAG. This
module is the single host reader of that protocol (the old copy-pasted BMPX
readers in bounce/plot/gfx tests all route here now), plus a small pixel model
for asserting that what reached the panel actually looks right.
"""
from __future__ import annotations

import os
import struct
import time
from dataclasses import dataclass
from typing import Optional, Tuple

from .session import DeviceSession, P4Error

DEFAULT_OUT = os.path.join(os.getcwd(), "screenshots")


@dataclass
class Bmp:
    width: int
    height: int
    bpp: int
    top_down: bool
    data: bytes          # raw pixel bytes
    stride: int
    data_offset: int

    def px(self, x: int, y: int) -> Tuple[int, int, int]:
        """Return (r, g, b) at (x, y), origin top-left."""
        row = y if self.top_down else (self.height - 1 - y)
        off = self.data_offset + row * self.stride + x * (self.bpp // 8)
        b = self.data[off]
        g = self.data[off + 1]
        r = self.data[off + 2]
        return r, g, b

    def region_mean(self, x: int, y: int, w: int, h: int) -> Tuple[float, float, float]:
        sr = sg = sb = 0
        n = 0
        for yy in range(max(0, y), min(self.height, y + h)):
            for xx in range(max(0, x), min(self.width, x + w)):
                r, g, b = self.px(xx, yy)
                sr += r
                sg += g
                sb += b
                n += 1
        if n == 0:
            return (0.0, 0.0, 0.0)
        return (sr / n, sg / n, sb / n)

    def count_near(self, x: int, y: int, w: int, h: int,
                   rgb: Tuple[int, int, int], tol: int = 24) -> int:
        tr, tg, tb = rgb
        n = 0
        for yy in range(max(0, y), min(self.height, y + h)):
            for xx in range(max(0, x), min(self.width, x + w)):
                r, g, b = self.px(xx, yy)
                if abs(r - tr) <= tol and abs(g - tg) <= tol and abs(b - tb) <= tol:
                    n += 1
        return n

    def to_image(self):
        from PIL import Image
        img = Image.new("RGB", (self.width, self.height))
        px = img.load()
        for yy in range(self.height):
            for xx in range(self.width):
                px[xx, yy] = self.px(xx, yy)
        return img

    def save_png(self, path: str) -> str:
        self.to_image().save(path)
        return path


def _parse_bmp(data: bytes) -> Bmp:
    if len(data) < 54 or data[0:2] != b"BM":
        raise P4Error("not a BMP (%r)" % data[:4])
    data_offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    top_down = height < 0
    height = abs(height)
    stride = ((width * bpp // 8 + 3) // 4) * 4
    return Bmp(width, height, bpp, top_down, data, stride, data_offset)


def capture(dev: DeviceSession, out_dir: Optional[str] = None,
            name: Optional[str] = None, timeout: float = 40.0) -> Bmp:
    """Grab the live screen (works while a modal/editor is open)."""
    dev.reset_input()
    dev.write_line("screenshot")
    prelude = dev.read_until(b"streaming", 15.0)
    if b"streaming" not in prelude:
        raise P4Error("screenshot: no streaming prelude; got %r" % prelude[-200:])
    # consume the rest of the "... bytes to serial...\r\n" line so the next
    # four bytes really are the BMPX magic.
    dev.read_until(b"\n", 5.0)
    raw = dev.read_binary_frame(b"BMPX", 15.0, timeout)
    bmp = _parse_bmp(raw)
    if out_dir is not None:
        os.makedirs(out_dir, exist_ok=True)
        stamp = name or time.strftime("shot_%Y%m%d_%H%M%S")
        path = os.path.join(out_dir, stamp + ".bmp")
        with open(path, "wb") as fh:
            fh.write(raw)
        with open(os.path.join(out_dir, "latest.bmp"), "wb") as fh:
            fh.write(raw)
        try:
            bmp.save_png(path[:-4] + ".png")
        except Exception:
            pass
    return bmp


def diff_ratio(a: Bmp, b: Bmp, tol: int = 16) -> float:
    """Fraction of sampled pixels that differ by more than ``tol``."""
    if a.width != b.width or a.height != b.height:
        return 1.0
    diff = 0
    total = 0
    for yy in range(0, a.height, 4):
        for xx in range(0, a.width, 4):
            pa = a.px(xx, yy)
            pb = b.px(xx, yy)
            if (abs(pa[0] - pb[0]) > tol or abs(pa[1] - pb[1]) > tol
                    or abs(pa[2] - pb[2]) > tol):
                diff += 1
            total += 1
    return diff / max(1, total)
