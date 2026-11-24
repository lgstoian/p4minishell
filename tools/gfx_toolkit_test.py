#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gfx_toolkit_test.py - HW verification for the B2 gfx toolkit.

Runs the GFXTOOL.BAT reference app (raster primitives + on-canvas text),
pulls the saved GFXTOOL.BMP over `send`, and checks representative pixels
against the RGB565 quantization the firmware applies.

Usage: python tools/gfx_toolkit_test.py [COMx]
"""
import os
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
from bg_run import boot, run_quiet  # noqa: E402
from p4test.screenshot import TOL_EXACT, parse_bmp  # noqa: E402

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


# The BMP pixel model and its comparators are the ones in p4test.screenshot;
# a saved RGB565 frame is compared byte-exactly (TOL_EXACT), not with a private
# threshold or a second top-down reader.


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    out = os.path.join(tempfile.gettempdir(), "p4minishell", "GFXTOOL.BMP")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sh = boot(port)
    img = False
    try:
        run = run_quiet(sh, "GFXTOOL.BAT", timeout=45)
        check("marker start", "[M-GFXTOOL]" in run, True)
        check("marker done", "[M-GFXTOOL-DONE]" in run, True)
        check("saved", "gfx: saved" in run, True)
        check("image marker", ("[M-GFXTOOL-IMG]" in run) or
              ("[M-GFXTOOL-NOIMG]" in run), True)
        img = "[M-GFXTOOL-IMG]" in run
        if not img:
            print("  note: PHOTO.BMP absent - BMP blit region not checked")
    finally:
        sh.close()

    # Fetch the saved BMP with the proven pull driver (reboots; file is on SD).
    last = ""
    for _ in range(3):
        time.sleep(2)
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "pull.py"),
                            "GFXTOOL.BMP", out, port],
                           capture_output=True, text=True)
        if "saved" in r.stdout:
            break
        last = "rc=%d out=%r err=%r" % (r.returncode, r.stdout[-200:], r.stderr[-200:])
    else:
        FAILS.append("pull")
        print("  [FAIL] pull GFXTOOL.BMP: %s" % last)
        return

    with open(out, "rb") as fh:
        b = parse_bmp(fh.read())
    print("  BMP %dx%d offset=%d" % (b.width, b.height, b.data_offset))
    check("dims", (b.width, b.height), (320, 200))
    check("background", b.px(10, 10), q(0x101820))
    check("border", b.px(4, 100), q(0x3A6EA5))
    check("triangle fill", b.px(40, 60), q(0xFF5555))
    check("ellipse fill", b.px(185, 95), q(0x55FF55))
    check("polygon fill", b.px(257, 70), q(0x5599FF))
    check("flood fill", b.px(266, 30), q(0xFFAA00))
    check("hline", b.px(100, 190), q(0xFFFFFF))
    check("vline", b.px(158, 100), q(0xFFFF00))
    check("text scale2 white",
          b.count_near(18, 150, 200 - 18, 166 - 150, q(0xFFFFFF),
                       tol=TOL_EXACT) > 0, True)
    check("text scale1 cyan",
          b.count_near(18, 170, 220 - 18, 178 - 170, q(0x00FFFF),
                       tol=TOL_EXACT) > 0, True)
    if img:
        # The scaled PHOTO.BMP blit lives in the lower-right free region; at
        # least one pixel there must differ from the background.
        check("bmp blit region", b.px(240, 160) != q(0x101820), True)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
