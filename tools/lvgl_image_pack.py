#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""lvgl_image_pack.py - pack PNG/JPEG images into LVGL v9 `.bin` + index.json.

Produces the LVGL v9 image-bin format (magic 0x19, `LV_COLOR_FORMAT_RGB565A8`)
and an `imageSet` index, the same shape ESP-Brookesia's
`gui/brookesia_gui_lvgl/cmake/image_pack.cmake` writes for its JSON-UI image
resources. The firmware side needs no second decoder: LVGL's own image decoder
consumes the `.bin` directly.

RGB565A8 layout (LVGL v9): the RGB565 color array (2 bytes/px, stride = 2*w)
followed by the 8-bit alpha array (1 byte/px, stride = w).

Usage:
  python tools/lvgl_image_pack.py --input assets/img --output build/img
  python tools/lvgl_image_pack.py --input assets/img --output build/img \
      --c-header assets/images_packed.h

`--c-header` additionally emits `lv_image_dsc_t` descriptors (one per image)
so firmware can reference them directly instead of loading the `.bin`.
"""

import argparse
import json
import os
import struct
import sys

from PIL import Image

LV_IMAGE_HEADER_MAGIC = 0x19
LV_COLOR_FORMAT_RGB565A8 = 0x14
IMAGE_EXTS = (".png", ".PNG", ".jpg", ".JPG", ".jpeg", ".JPEG")


def bin_header(w, h, stride):
    """12-byte LVGL v9 image header: magic, cf, flags, w, h, stride, reserved."""
    return struct.pack("<BBHHHHH", LV_IMAGE_HEADER_MAGIC,
                       LV_COLOR_FORMAT_RGB565A8, 0, w, h, stride, 0)


def rgb565a8(img):
    """Return (bytes, stride) for an RGB565A8-encoded image."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    px = rgba.load()
    stride = w * 2
    color = bytearray(stride * h)
    alpha = bytearray(w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, _a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            color[i] = v & 0xFF
            color[i + 1] = (v >> 8) & 0xFF
            i += 2
    k = 0
    for y in range(h):
        for x in range(w):
            alpha[k] = px[x, y][3]
            k += 1
    return bytes(color) + bytes(alpha), stride


def image_id(name):
    """`sub/dir/My-Icon.png` -> `my_icon` (LVGL image ids are dotted names)."""
    base = os.path.splitext(os.path.basename(name))[0]
    out = []
    for ch in base:
        if ch.isalnum():
            out.append(ch.lower())
        else:
            out.append("_")
    ident = "".join(out).strip("_")
    return ident or "image"


def main():
    ap = argparse.ArgumentParser(description="Pack images into LVGL v9 .bin + index.json.")
    ap.add_argument("--input", "-i", required=True, help="input image directory")
    ap.add_argument("--output", "-o", required=True, help="output directory")
    ap.add_argument("--c-header", help="optional C header with lv_image_dsc_t descriptors")
    args = ap.parse_args()

    if not os.path.isdir(args.input):
        print("FAIL: input dir not found: %s" % args.input)
        return 1

    os.makedirs(args.output, exist_ok=True)
    names = []
    for base, _dirs, files in os.walk(args.input):
        for name in files:
            if name.endswith(IMAGE_EXTS):
                names.append(os.path.join(base, name))
    names.sort()
    if not names:
        print("FAIL: no PNG/JPEG images under %s" % args.input)
        return 1

    index = {"type": "imageSet", "images": []}
    descriptors = []
    for path in names:
        rel = os.path.relpath(path, args.input).replace(os.sep, "/")
        ident = image_id(rel)
        img = Image.open(path)
        img.load()
        data, stride = rgb565a8(img)
        bin_name = ident + ".bin"
        with open(os.path.join(args.output, bin_name), "wb") as f:
            f.write(bin_header(img.width, img.height, stride))
            f.write(data)
        index["images"].append({
            "id": ident,
            "src": bin_name,
            "width": img.width,
            "height": img.height,
        })
        descriptors.append((ident, img.width, img.height, stride, len(data)))
        print("  %-28s %4dx%-4d %6d bytes" % (rel, img.width, img.height, len(data) + 12))

    with open(os.path.join(args.output, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=2)
        f.write("\n")

    if args.c_header:
        guard = "P4MINISHELL_PACKED_IMAGES_H"
        lines = [
            "/**",
            " * @file %s" % os.path.basename(args.c_header),
            " * @brief Generated packed images (see tools/lvgl_image_pack.py).",
            " *",
            " * Do not edit by hand: re-run `python tools/lvgl_image_pack.py`.",
            " */",
            "",
            "#ifndef %s" % guard,
            "#define %s" % guard,
            "",
            '#include "lvgl.h"',
            "",
            "#ifdef __cplusplus",
            'extern "C" {',
            "#endif",
            "",
        ]
        for ident, w, h, stride, size in descriptors:
            var = "p4_img_" + ident
            lines.append("/** %d x %d RGB565A8 packed image. */" % (w, h))
            lines.append("extern const lv_image_dsc_t %s;" % var)
            lines.append("")
        lines += [
            "#ifdef __cplusplus",
            "}",
            "#endif",
            "",
            "#endif /* %s */" % guard,
            "",
        ]
        os.makedirs(os.path.dirname(args.c_header) or ".", exist_ok=True)
        with open(args.c_header, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
        print("wrote %s (%d descriptors)" % (args.c_header, len(descriptors)))

    print("wrote %s/index.json (%d image(s))" % (args.output, len(index["images"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
