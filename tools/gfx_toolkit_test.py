#!/usr/bin/env python3
"""gfx_toolkit_test.py - HW verification for the B2 gfx toolkit.

Runs the GFXTOOL.BAT reference app (raster primitives + on-canvas text),
pulls the saved GFXTOOL.BMP over `send`, and checks representative pixels
against the RGB565 quantization the firmware applies.

Usage: python tools/gfx_toolkit_test.py [COMx]
"""
import struct
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot, run_quiet  # noqa: E402

FAILS = []


def q(rgb):
    """RGB888 -> RGB565 -> RGB888 (the firmware's quantization)."""
    r = (rgb >> 16) & 0xFF
    g = (rgb >> 8) & 0xFF
    b = rgb & 0xFF
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)


def check(name, got, want):
    ok = got == want
    print("  [%s] %s got=%s want=%s" % ("PASS" if ok else "FAIL", name, got, want))
    if not ok:
        FAILS.append(name)


class Bmp:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        assert self.data[:2] == b"BM", "not a BMP"
        self.off = struct.unpack("<I", self.data[10:14])[0]
        self.w = struct.unpack("<i", self.data[18:22])[0]
        self.h = struct.unpack("<i", self.data[22:26])[0]
        self.stride = (self.w * 3 + 3) & ~3

    def px(self, x, y):
        """Top-down (x, y) -> (R, G, B); BMP rows are bottom-up."""
        base = self.off + (self.h - 1 - y) * self.stride + x * 3
        b, g, r = self.data[base], self.data[base + 1], self.data[base + 2]
        return (r, g, b)

    def count(self, x0, y0, x1, y1, want):
        n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                if self.px(x, y) == want:
                    n += 1
        return n


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    out = r"C:\Users\lgstoian\AppData\Local\Temp\opencode\GFXTOOL.BMP"
    sh = boot(port)
    try:
        run = run_quiet(sh, "GFXTOOL.BAT", timeout=45)
        check("marker start", "[M-GFXTOOL]" in run, True)
        check("marker done", "[M-GFXTOOL-DONE]" in run, True)
        check("saved", "gfx: saved" in run, True)
    finally:
        sh.close()

    # Fetch the saved BMP with the proven pull driver (reboots; file is on SD).
    last = ""
    for _ in range(3):
        time.sleep(2)
        r = subprocess.run([sys.executable, "tools/pull.py", "GFXTOOL.BMP", out, port],
                           capture_output=True, text=True)
        if "saved" in r.stdout:
            break
        last = "rc=%d out=%r err=%r" % (r.returncode, r.stdout[-200:], r.stderr[-200:])
    else:
        FAILS.append("pull")
        print("  [FAIL] pull GFXTOOL.BMP: %s" % last)
        return

    b = Bmp(out)
    print("  BMP %dx%d offset=%d" % (b.w, b.h, b.off))
    check("dims", (b.w, b.h), (320, 200))
    check("background", b.px(10, 10), q(0x101820))
    check("border", b.px(4, 100), q(0x3A6EA5))
    check("triangle fill", b.px(40, 60), q(0xFF5555))
    check("ellipse fill", b.px(185, 95), q(0x55FF55))
    check("polygon fill", b.px(257, 70), q(0x5599FF))
    check("flood fill", b.px(266, 30), q(0xFFAA00))
    check("hline", b.px(100, 190), q(0xFFFFFF))
    check("vline", b.px(158, 100), q(0xFFFF00))
    check("text scale2 white", b.count(18, 150, 200, 166, q(0xFFFFFF)) > 0, True)
    check("text scale1 cyan", b.count(18, 170, 220, 178, q(0x00FFFF)) > 0, True)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
