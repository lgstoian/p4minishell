#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_icon.py - generate the repo icon and the firmware boot-splash asset.

Source of truth: ``icon/icon.png`` (the author's master; it is large and is
git-ignored). This script produces:

  * ``icon/icon-512.png``            - committed, referenced by readme.md
  * ``main/assets/icon_splash.c/.h`` - committed, a 256x256 RGB565
                                       ``lv_image_dsc_t`` for the boot splash

The firmware asset is flattened onto the default theme screen colour (the
splash is opaque - no alpha format). Re-run after replacing the master:

    python tools/make_icon.py

If the master is absent the generated files are left untouched and the script
exits 0 (a fresh checkout still builds from the committed assets).
"""

import os
import sys

from PIL import Image

# The 16384x16384 master trips Pillow's decompression-bomb guard; it is our own
# file, so lift the limit.
Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "icon", "icon.png")
README_PNG = os.path.join(ROOT, "icon", "icon-512.png")
ASSET_C = os.path.join(ROOT, "main", "assets", "icon_splash.c")
ASSET_H = os.path.join(ROOT, "main", "assets", "icon_splash.h")

SPLASH_PX = 256
README_PX = 512
# Default theme screen background (#0B0F10) - the firmware asset is flattened
# onto it so the RGB565 splash blends with the backdrop.
BG = (0x0B, 0x0F, 0x10)


def flatten_rgb(im, size, bg):
    """Downscale to size x size and composite any alpha onto @p bg (opaque)."""
    small = im.resize((size, size), Image.LANCZOS)
    out = Image.new("RGB", (size, size), bg)
    if small.mode in ("RGBA", "LA"):
        out.paste(small, mask=small.split()[-1])
    elif small.mode != "RGB":
        small = small.convert("RGB")
        out.paste(small)
    else:
        out.paste(small)
    return out


def rgb565_le_bytes(rgb):
    """Little-endian RGB565 bytes for an RGB image."""
    px = rgb.load()
    data = bytearray(rgb.width * rgb.height * 2)
    i = 0
    for y in range(rgb.height):
        for x in range(rgb.width):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            data[i] = v & 0xFF
            data[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(data)


def write_header():
    with open(ASSET_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/**\n"
            " * @file icon_splash.h\n"
            " * @brief Generated boot-splash image (see tools/make_icon.py).\n"
            " *\n"
            " * Do not edit by hand: re-run `python tools/make_icon.py` from the\n"
            " * repository root after replacing icon/icon.png.\n"
            " */\n\n"
            "#ifndef P4MINISHELL_ICON_SPLASH_H\n"
            "#define P4MINISHELL_ICON_SPLASH_H\n\n"
            "#include \"lvgl.h\"\n\n"
            "#ifdef __cplusplus\n"
            "extern \"C\" {\n"
            "#endif\n\n"
            "/** %d x %d RGB565 boot-splash image. */\n"
            "extern const lv_image_dsc_t p4_icon_splash;\n\n"
            "#ifdef __cplusplus\n"
            "}\n"
            "#endif\n\n"
            "#endif /* P4MINISHELL_ICON_SPLASH_H */\n" % (SPLASH_PX, SPLASH_PX)
        )


def write_source(data):
    with open(ASSET_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/**\n"
            " * @file icon_splash.c\n"
            " * @brief Generated boot-splash image (see tools/make_icon.py).\n"
            " *\n"
            " * Do not edit by hand: re-run `python tools/make_icon.py` from the\n"
            " * repository root after replacing icon/icon.png.\n"
            " */\n\n"
            "#include \"icon_splash.h\"\n\n"
            "static const uint8_t p4_icon_splash_map[] = {\n"
        )
        for i in range(0, len(data), 16):
            row = data[i:i + 16]
            f.write("    " + " ".join("0x%02x," % b for b in row) + "\n")
        f.write(
            "};\n\n"
            "const lv_image_dsc_t p4_icon_splash = {\n"
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "    .header.cf = LV_COLOR_FORMAT_RGB565,\n"
            "    .header.w = %d,\n"
            "    .header.h = %d,\n"
            "    .header.stride = %d,\n"
            "    .data_size = sizeof(p4_icon_splash_map),\n"
            "    .data = p4_icon_splash_map,\n"
            "};\n" % (SPLASH_PX, SPLASH_PX, SPLASH_PX * 2)
        )


def main():
    if not os.path.exists(SRC):
        print("icon master not found (%s); keeping committed assets" % SRC)
        return 0

    os.makedirs(os.path.dirname(ASSET_C), exist_ok=True)

    im = Image.open(SRC)
    im.load()

    # Committed readme icon keeps the alpha channel.
    readme = im.resize((README_PX, README_PX), Image.LANCZOS)
    readme.save(README_PNG)
    print("wrote %s (%d x %d)" % (os.path.relpath(README_PNG, ROOT), README_PX, README_PX))

    small = flatten_rgb(im, SPLASH_PX, BG)
    data = rgb565_le_bytes(small)
    write_header()
    write_source(data)
    print("wrote %s / icon_splash.h (%d x %d RGB565, %d bytes)"
          % (os.path.relpath(ASSET_C, ROOT), SPLASH_PX, SPLASH_PX, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
