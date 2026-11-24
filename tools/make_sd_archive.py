#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_sd_archive.py - assemble the complete SD-card payload for a release.

The same manifest the push tools use (`apps/app_manifest.py`) is the source of
truth for the app tree; templates, dictionaries and reading fonts are added
from their owning folders. Nothing is invented: every file in the archive is a
repository asset that a push tool would place on the card.

Usage:
    python tools/make_sd_archive.py [--out release/p4minishell-sd-v1.5.0.zip]
                                    [--fonts all|dejavu|none]
                                    [--dry-run]
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "apps"))
from app_manifest import deploy_list  # noqa: E402

README = """P4MiniShell v1.5.0 - SD card payload
=====================================

Copy the CONTENTS of this folder to the root of a FAT32 microSD card (or a USB
stick). The shell finds the apps on PATH (the card root) and their metadata in
APPS/. WEB/ is the browser showcase, EMOJI/ the sample emoji pack, TEMPLATES/
and DICTS/ feed the editor/writerdeck, FONTS/ adds reading typography.

Quick start: insert the card, boot, type `launch /list`.

Every file here is project-authored or a bundled third-party asset; see
licence.md in the source repository.
"""


def add_file(sd_dir: str, remote: str, local: str) -> bool:
    dest = os.path.join(sd_dir, remote.replace("/", os.sep))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(local, dest)
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(
        ROOT, "release", "p4minishell-sd-v1.5.0.zip"))
    ap.add_argument("--fonts", choices=("all", "dejavu", "none"), default="dejavu")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="p4sd_")
    sd_dir = os.path.join(tmp, "SD")
    os.makedirs(sd_dir, exist_ok=True)
    count = 0

    for local, remote in deploy_list():
        if not os.path.isfile(local):
            print("MISSING payload:", local)
            return 1
        add_file(sd_dir, remote, local)
        count += 1

    # Templates and dictionaries (their push tools use these folders directly).
    for folder, remote_dir, suffix in (
        (os.path.join(ROOT, "apps", "templates"), "TEMPLATES", ".MD"),
        (os.path.join(ROOT, "apps", "dicts"), "DICTS", ".words"),
    ):
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if not name.endswith(suffix):
                continue
            add_file(sd_dir, remote_dir + "/" + name, os.path.join(folder, name))
            count += 1

    # Reading fonts. NotoSansSC is 17 MB and optional; DejaVu is the default
    # reading face the firmware auto-selects when present.
    if args.fonts != "none":
        fonts = os.path.join(ROOT, "assets", "fonts")
        for name in sorted(os.listdir(fonts)) if os.path.isdir(fonts) else []:
            if not name.lower().endswith((".ttf", ".otf")):
                continue
            if args.fonts == "dejavu" and not name.startswith("DejaVu"):
                continue
            add_file(sd_dir, "FONTS/" + name, os.path.join(fonts, name))
            count += 1

    with open(os.path.join(sd_dir, "README.TXT"), "w", encoding="utf-8") as fh:
        fh.write(README)

    if args.dry_run:
        print("would write %d files from %s" % (count, sd_dir))
        print("archive:", args.out)
        return 0

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for base, _dirs, files in os.walk(sd_dir):
            for name in files:
                path = os.path.join(base, name)
                rel = os.path.relpath(path, sd_dir)
                zf.write(path, "SD/" + rel.replace(os.sep, "/"))
    shutil.rmtree(tmp, ignore_errors=True)
    print("wrote %s (%d payload files, %.1f MB)"
          % (args.out, count, os.path.getsize(args.out) / (1024.0 * 1024.0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
