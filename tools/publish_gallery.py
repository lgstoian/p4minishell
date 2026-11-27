#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Publish staged gallery captures to docs/assets/apps, but only after the
identity audit passes. A test run NEVER writes documentation directly: it
stages into screenshots/gallery/, and this explicit command is the only path
that updates the committed PNG set.

Usage:
    python tools/publish_gallery.py           # audit + show what would change
    python tools/publish_gallery.py --yes     # perform the copy
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys

from gallery_audit import GALLERY, audit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Some files under docs/assets/apps are OWNED by a deterministic host proof
# generator, not by a board capture. A gallery publish must never overwrite
# them: make_gfx_proof.py --check verifies exact pixel/colour baselines, so a
# board capture at this path silently broke the host gate. Parent owns the
# proof; the gallery keeps every other staged capture.
PROOF_OWNED = frozenset({"gfxtool.png"})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--yes", action="store_true", help="actually copy the files")
    args = ap.parse_args()

    staged = os.path.join(ROOT, "screenshots", "gallery")
    if not os.path.isdir(staged):
        print("publish_gallery: no staged captures in %s" % staged)
        return 1

    # A wrong capture must never reach docs/, even if the operator says yes.
    rc = audit(staged)
    if rc != 0:
        print("publish_gallery: staged captures failed the identity audit")
        return rc

    all_files = sorted(f for f in os.listdir(staged) if f.lower().endswith(".png"))
    skipped = sorted(f for f in all_files if f in PROOF_OWNED)
    files = [f for f in all_files if f not in PROOF_OWNED]
    if not files:
        print("publish_gallery: staging folder has no publishable captures")
        return 1
    if skipped:
        print("publish_gallery: preserving proof-owned file(s): %s"
              % ", ".join(skipped))

    changed = []
    for name in files:
        src = os.path.join(staged, name)
        dst = os.path.join(GALLERY, name)
        if not os.path.exists(dst) or open(src, "rb").read() != open(dst, "rb").read():
            changed.append(name)

    print("publish_gallery: %d capture(s), %d would change" % (len(files), len(changed)))
    for name in changed:
        print("  %s" % name)
    if not args.yes:
        print("publish_gallery: dry run - pass --yes to copy")
        return 0

    os.makedirs(GALLERY, exist_ok=True)
    for name in files:
        shutil.copyfile(os.path.join(staged, name), os.path.join(GALLERY, name))
    print("publish_gallery: published %d PNG(s)" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
