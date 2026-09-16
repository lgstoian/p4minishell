#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gfx_sprite_test.py - HW verify gfx load/blt/save/slots/free + errors.

Usage: python gfx_sprite_test.py [COMx]
Builds a 32x32 24-bit BMP on the host (PIL), pushes it via the receive
protocol, then drives every new verb incl. error paths. Screenshots the
blit result for visual proof.
"""
import os
import struct
import sys
import time

sys.path.insert(0, "tools")
sys.path.insert(0, os.path.join("apps", "companion"))
from push_sd import push_file, wait_shell  # noqa: E402
from bg_run import run_quiet, boot  # noqa: E402
from shell_session import open_port, hard_reset  # noqa: E402


def make_bmp(path):
    from PIL import Image
    im = Image.new("RGB", (32, 32), (0, 0, 170))
    px = im.load()
    for y in range(8, 24):
        for x in range(8, 24):
            px[x, y] = (255, 85, 85)
    px[0, 0] = (255, 255, 255)
    im.save(path)
    return os.path.getsize(path)


def shot(sh, out):
    sh.s.reset_input_buffer()
    sh.s.write(b"screenshot\r\n")
    data = b""
    end = time.time() + 60
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"BMPX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                while len(data) < i + 8 + size and time.time() < end:
                    more = sh.s.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open(out, "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                print("saved %s" % out, flush=True)
                return
    print("screenshot TIMEOUT", flush=True)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    bmp = os.path.join("screenshots", "spr32.bmp")
    os.makedirs("screenshots", exist_ok=True)
    print("host bmp bytes: %d" % make_bmp(bmp), flush=True)

    hard_reset(port)  # open_port() no longer resets; start from a clean prompt
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: no shell", flush=True)
        return
    time.sleep(20.0)
    ser.reset_input_buffer()
    with open(bmp, "rb") as f:
        data = f.read()
    print("push SPR32.BMP: %s" % push_file(ser, "SPR32.BMP", data), flush=True)
    ser.close()

    sh = boot(port)
    run_quiet(sh, "dir SPR32.BMP")
    cmds = [
        "gfx init 240 180",
        "gfx load 0 SPR32.BMP",
        "gfx slots",
        "gfx load 9 SPR32.BMP",
        "gfx load 1 NOTES.BAT",
        "gfx blit 0 100 60",
        "gfx blit 1 10 10",
        "gfx blit 0 200 140 0",
        "gfx show",
        "gfx save SPR_OUT.BMP",
        "gfx free 0",
        "gfx slots",
        "gfx blit 0 10 10",
        "gfx close",
    ]
    for cmd in cmds:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-350:], flush=True)
    shot(sh, "screenshots/gfx_spr_bl.bmp")
    sh.close()


main()
