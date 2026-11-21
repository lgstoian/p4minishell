#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Publish staged live-browser proof to docs/assets/proof.

Companion to tools/publish_gallery.py: the live Wikipedia leg of s21 stages its
capture under screenshots/proof/. This command is the only path that copies it
into documentation, and it verifies that the stage contains both a render
dump and a non-trivial screenshot before copying.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(ROOT, "screenshots", "proof")
DEST = os.path.join(ROOT, "docs", "assets", "proof")
APPS = os.path.join(ROOT, "docs", "assets", "apps")


def main() -> int:
    dump = os.path.join(STAGE, "wikipedia_live.txt")
    image = os.path.join(STAGE, "wikipedia_live.png")
    for path in (dump, image):
        if not os.path.isfile(path) or os.path.getsize(path) < 1000:
            print("publish_proof: missing or empty %s" % path)
            return 1
    text = open(dump, encoding="utf-8").read()
    for token in ("https://en.wikipedia.org/wiki/ESP32", "Espressif", "ESP32"):
        if token not in text:
            print("publish_proof: dump lacks %r" % token)
            return 1

    os.makedirs(DEST, exist_ok=True)
    for name in ("wikipedia_live.png", "wikipedia_live.txt", "web.png"):
        shutil.copyfile(os.path.join(STAGE, name), os.path.join(DEST, name))
    # The documented `web` app proof is the same live app screenshot.
    shutil.copyfile(os.path.join(STAGE, "wikipedia_live.png"),
                    os.path.join(APPS, "web.png"))
    print("publish_proof: published live Wikipedia proof (3 files + web.png)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
