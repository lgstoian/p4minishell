#!/usr/bin/env python3
"""push_assets.py - push game/app asset blobs + CRC manifests.

Generates deterministic sample sprites with PIL (no binary blobs in the
repo), pushes them with the proven receive/CRC path, and writes
APPS/<APP>.ASSETS manifests (path=HEXCRC lines) verifiable on-device via
`asset check <app>` (zlib CRC parity with the firmware's shell_crc32).

Also emits manifests for the reference .BAT apps (pushed by push_apps.py)
so `asset check BOUNCE|SNAKE|TCMD|ELITE` covers them too.

Usage: python push_assets.py [COMx]
"""
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import push_file, wait_shell, open_port  # noqa: E402

import push_apps  # noqa: E402
import push_sd  # noqa: E402

APPS_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.getcwd(), "assets_out")
os.makedirs(OUT_DIR, exist_ok=True)


def make_ship(path):
    """48x48 saucer on CGA blue: hull ellipse + cabin + lights."""
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (48, 48), (0, 0, 170))
    d = ImageDraw.Draw(im)
    d.ellipse([6, 20, 42, 34], fill=(170, 170, 170))
    d.ellipse([18, 12, 30, 24], fill=(85, 255, 255))
    d.point([(10, 27), (24, 27), (38, 27)], fill=(255, 85, 85))
    d.rectangle([22, 34, 26, 38], fill=(85, 85, 85))
    im.save(path)


def make_ball(path):
    """16x16 BALL: white disc with red core on black (transparency demo)."""
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (16, 16), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([1, 1, 14, 14], fill=(255, 255, 255))
    d.ellipse([5, 5, 10, 10], fill=(255, 0, 0))
    im.save(path)


def make_photo(path):
    """96x64 PHOTO.BMP: a gradient sky + sun + hills, for the image viewer,
    `draw image` TUI render, and `gfx image` canvas blit (PICS.BAT)."""
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (96, 64))
    px = im.load()
    for y in range(64):
        for x in range(96):
            # Vertical gradient: deep blue -> warm horizon.
            r = int(20 + (y / 63.0) * 200)
            g = int(60 + (y / 63.0) * 120)
            b = int(160 - (y / 63.0) * 120)
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(im)
    d.ellipse([64, 8, 84, 28], fill=(255, 230, 120))          # sun
    d.polygon([(0, 64), (30, 30), (60, 64)], fill=(30, 110, 45))   # hills
    d.polygon([(40, 64), (72, 36), (96, 64)], fill=(20, 80, 35))
    im.save(path)


SPRITES = [
    ("SHIP.BMP", make_ship),
    ("BALL.BMP", make_ball),
    ("PHOTO.BMP", make_photo),
]


def crc_hex(data):
    return "%08X" % (zlib.crc32(data) & 0xFFFFFFFF)


def push_one(ser, target, data):
    for attempt in range(3):
        if push_file(ser, target, data):
            print("PUSH ok   %s (%d bytes)" % (target, len(data)), flush=True)
            return True
        print("  retry %s (%d/3)" % (target, attempt + 1), flush=True)
        time.sleep(2.0)
    return False


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    # 1. Generate sprites locally.
    blobs = {}
    for name, maker in SPRITES:
        path = os.path.join(OUT_DIR, name)
        maker(path)
        with open(path, "rb") as f:
            blobs[name] = f.read()
        print("GEN  %s (%d bytes)" % (path, len(blobs[name])), flush=True)
    # 2. Manifests for the .BAT apps (bytes identical to push_apps sources).
    manifests = {}
    for subdir, name, appinfo in push_apps.FILES:
        app = os.path.splitext(name)[0]
        with open(os.path.join(APPS_DIR, subdir, name), "rb") as f:
            data = f.read()
        target = ("APPS/" + name) if appinfo else name
        manifests.setdefault(app, []).append((target, crc_hex(data)))
    manifests.setdefault("SPR", []).extend(
        (name, crc_hex(blobs[name])) for name, _ in SPRITES)
    # Companion (pushed by apps/companion/push_sd.py): root BATs + README plus
    # the APPS/COMPANION.APPINFO metadata.
    for name in push_sd.FILES:
        with open(os.path.join(APPS_DIR, "companion", name), "rb") as f:
            data = f.read()
        target = ("APPS/" + name) if name.endswith(".APPINFO") else name
        manifests.setdefault("COMPANION", []).append((target, crc_hex(data)))
    # 3. Push sprites + manifests.
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port, flush=True)
        ser.close()
        return 1
    time.sleep(20.0)
    ser.reset_input_buffer()
    ser.write(b"md APPS\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)
    ok = True
    for name, _ in SPRITES:
        ok = push_one(ser, name, blobs[name]) and ok
    for app, entries in sorted(manifests.items()):
        if app == "ALIASES":
            continue  # three same-named sources overwrite one target; unattributable
        body = ("; %s assets (push_assets.py; verify: asset check %s)\n"
                % (app, app)).encode()
        for target, crc in entries:
            body += ("%s=%s\n" % (target, crc)).encode()
        ok = push_one(ser, "APPS/%s.ASSETS" % app, body) and ok
    ser.close()
    print("RESULT %s" % ("OK" if ok else "FAIL"), flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
