#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""visual_proof_gate.py - judge the VISUAL QUALITY of committed proof PNGs.

A marker-only gate can go green on a blank frame, a shell transcript dressed up
as an app, or two pages that are byte-identical. This gate closes that gap for
the committed 1024x600 HOST proof images. It reads every pixel through the ONE
host pixel model - ``p4test.screenshot.Bmp`` (with ``p4test.visual`` analytics,
``p4test.theme_guard`` and the palette loader in ``make_webapp_proof``) - and
never copies a second BMP reader, palette or comparator.

What --check asserts for every proof frame:
  * exact 1024x600 RGB container;
  * DOS palette usage: a real share of the non-background pixels matches the
    shipped ``p4minishell_config.h`` palette (not just the dark background);
  * CP437 box-frame continuity: long horizontal and vertical single-colour
    runs at the panel borders (for the web page proofs);
  * ink in the expected header / grid / status bands (a blank grid is red);
  * no LVGL default-theme blue blobs and no empty-box (tofu) glyph cells;
  * no transcript-marker dump dressed up as a dashboard - more than 40% of the
    grid ink in `[M-...]` rows is rejected (template-matched with the ONE
    shipped glyph renderer, not a second OCR model);
  * a distinct signature for every page/app, no two identical screens, and a
    byte-exact docs mirror of each web page;
  * the committed signature of each named proof still matches its pixels.

PROVENANCE: these are HOST proofs, not device captures. A pass is a board-free
pre-gate only; ``harness.md`` section 0 still governs hardware sign-off.

Usage:
    python tools/visual_proof_gate.py --check       # default
    python tools/visual_proof_gate.py --update      # re-sign passing proofs
    python tools/visual_proof_gate.py --self-test   # red canaries, no fixtures
"""
from __future__ import annotations

import argparse
import hashlib
import io
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
WEB = ROOT / "screenshots" / "web"
MIRROR = ROOT / "docs" / "assets" / "web"
# The committed app proofs produced by the shipped host proof builders. They
# are host renders, not board captures; device galleries have their own gate.
APP_PROOFS = (
    ROOT / "docs" / "assets" / "web-features.png",
    ROOT / "docs" / "assets" / "webapp-forms.png",
)

sys.path.insert(0, str(TOOLS))
from p4test import screenshot as shot, theme_guard, visual  # noqa: E402
import make_webapp_proof as wp  # noqa: E402 - palette + panel geometry

PANEL_W, PANEL_H = wp.PANEL_W, wp.PANEL_H
GRID_X, GRID_Y = wp.GRID_X, wp.GRID_Y
CELL_W, CELL_H = wp.CELL_W, wp.CELL_H
COLS, ROWS = wp.COLS, wp.ROWS
FOOTER_Y = wp.FOOTER_Y

# Thresholds are deliberately stated here, not hidden in the helpers: every
# one is exercised by a red canary in self_test().
PALETTE_TOL = 10
PALETTE_INK_MIN = 0.45
FRAME_GAP = 6                 # a cell gap in a CP437 rule is bridged
H_RUN_MIN = 300               # longest horizontal frame run (px)
H_ROWS_MIN = 2                # rows with a >= 200px horizontal frame run
V_RUN_MIN = 180               # longest vertical frame run (px)
V_COLS_MIN = 2                # columns with a >= 120px vertical frame run
INK_TOP_MIN = 400
INK_GRID_MIN = 1000
INK_FOOTER_MIN = 250
TOFU_MAX = 2                  # more than two empty-box cells is a defect
MARKER_INK_MAX = 0.40         # >40% of grid ink in [M-...] rows is a dump
MARKER_PREFIX = "[M-"         # the exact transcript-marker prefix
DUP_L1 = 8                    # same 24x24 signature grid as gallery_audit

# Committed SHA-256 of each proof's 24x24 grayscale signature. --update rewrites
# this block; --check refuses a missing or changed entry. Keeping the table in
# this one file means --update does not write outside the gate's own scope.
COMMITTED_SIGNATURES: Dict[str, str] = {
    "article.png": "2b2995be6076657b75acae1dfb7ff9846631d4506816692dcf5047d2da20546f",
    "dashboard.png": "31ff117e5e1a64ccd0ea3a3c64e1ecd9cb23a72d46af8ce488d6748da511be8d",
    "form.png": "2553a5d7818245f7ccbc5e86848e7f0cb73d8132f0a4f2bf3a87e09b1232f1ec",
    "portal.png": "e7a528c83def027d1c34fdf0e234ae9f7a9f0e768a6eca99ca83966ffe260d66",
    "web-features.png": "d8172be52d46c61bcac67bf8b4da8947bd6d8948648ba3d88ca8b7ab35cf2e6d",
    "webapp-forms.png": "b9cfcecc628df02cbb0724f8b31afdaa0bd8db87ef1014c1887d518ee66b3d54",
    "wiki_esp32.png": "16bb73158ae0be7707801b4cb9796c115d4fcf66288216edf14834f1ec8036ea",
}


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


# ---------------------------------------------------------------------------
# The ONE pixel model: PNG -> p4test.screenshot.Bmp, never a second reader.
# ---------------------------------------------------------------------------
def load_bmp(path: Path) -> shot.Bmp:
    if not path.is_file():
        raise GateError("proof frame is missing: %s" % path)
    from PIL import Image

    try:
        with Image.open(path) as img:
            buf = io.BytesIO()
            img.convert("RGB").save(buf, format="BMP")
    except Exception as exc:  # noqa: BLE001
        raise GateError("undecodable proof %s: %s" % (path, exc)) from exc
    bmp = shot.parse_bmp(buf.getvalue())
    if not isinstance(bmp, shot.Bmp):  # guard against a future model swap
        raise GateError("proof %s did not decode to p4test.screenshot.Bmp" % path)
    return bmp


def _palette() -> List[Tuple[int, int, int]]:
    """Read the shipped DOS palette through the existing proof builder."""
    try:
        return wp.load_palette()
    except Exception as exc:  # noqa: BLE001
        raise GateError("cannot load the shipped DOS palette: %s" % exc) from exc


# ---------------------------------------------------------------------------
# Analytics. All of them start from visual.array(Bmp); no PIL-only model.
# ---------------------------------------------------------------------------
def _longest_run(line, max_gap: int) -> int:
    """Longest True run in a boolean row, bridging gaps of at most max_gap."""
    import numpy as np

    gaps = np.flatnonzero(~line)
    if gaps.size == 0:
        return int(line.size)
    longest = start = 0
    i = 0
    while i < gaps.size:
        j = i + 1
        while j < gaps.size and int(gaps[j]) == int(gaps[j - 1]) + 1:
            j += 1
        g0, g1 = int(gaps[i]), int(gaps[j - 1])
        if g1 - g0 + 1 > max_gap:
            longest = max(longest, g0 - start)
            start = g1 + 1
        i = j
    return int(max(longest, line.size - start))


def frame_runs(bmp: shot.Bmp) -> Tuple[int, int, int, int]:
    """(max horizontal run, rows with long H runs, max vertical run, cols)."""
    arr = visual.array(bmp)
    mask = visual.nonbg_mask(arr, visual.bg_color(arr))
    h_long = [n for n in ( _longest_run(mask[y], FRAME_GAP)
                           for y in range(bmp.height)) if n >= 200]
    v_long = [n for n in ( _longest_run(mask[:, x], FRAME_GAP)
                           for x in range(bmp.width)) if n >= 120]
    return (max(h_long, default=0), len(h_long),
            max(v_long, default=0), len(v_long))


def ink_palette_share(bmp: shot.Bmp) -> float:
    """Share of INK pixels within PALETTE_TOL of a shipped palette colour."""
    import numpy as np

    arr = visual.array(bmp)
    ink = visual.nonbg_mask(arr, visual.bg_color(arr))
    pixels = arr[ink][::3].astype(np.int16)
    if pixels.size == 0:
        return 0.0
    pal = np.array(_palette(), dtype=np.int16)
    delta = np.abs(pixels[:, None, :] - pal[None, :, :]).max(axis=2)
    return float((delta <= PALETTE_TOL).any(axis=1).mean())


def ink_bands(bmp: shot.Bmp) -> Tuple[int, int, int]:
    arr = visual.array(bmp)
    mask = visual.nonbg_mask(arr, visual.bg_color(arr))
    top = int(mask[:GRID_Y, :].sum())
    grid = int(mask[GRID_Y:FOOTER_Y, :].sum())
    foot = int(mask[FOOTER_Y:, :].sum())
    return top, grid, foot


def empty_box_cells(bmp: shot.Bmp) -> int:
    """Count hollow rectangular glyph cells (missing-glyph / tofu boxes).

    A real CP437 corner or rule has only one or two edges; a tofu box paints
    all four edges and leaves the centre empty. Sampling the cell interior
    keeps anti-aliasing fringes from being counted.
    """
    arr = visual.array(bmp)
    mask = visual.nonbg_mask(arr, visual.bg_color(arr))
    tofu = 0
    for row in range(ROWS):
        cy = GRID_Y + row * CELL_H
        if cy + CELL_H > bmp.height:
            break
        for col in range(COLS):
            cx = GRID_X + col * CELL_W
            if cx + CELL_W > bmp.width:
                break
            top = float(mask[cy + 3:cy + 6, cx + 1:cx + CELL_W - 1].mean())
            bottom = float(mask[cy + CELL_H - 6:cy + CELL_H - 3,
                                cx + 1:cx + CELL_W - 1].mean())
            left = float(mask[cy + 2:cy + CELL_H - 2, cx + 1:cx + 4].mean())
            right = float(mask[cy + 2:cy + CELL_H - 2,
                               cx + CELL_W - 4:cx + CELL_W - 1].mean())
            centre = float(mask[cy + 7:cy + 13, cx + 4:cx + 9].mean())
            if min(top, bottom, left, right) >= 0.50 and centre <= 0.15:
                tofu += 1
    return tofu


def marker_row_flags(bmp: shot.Bmp) -> List[bool]:
    """Which rendered cell rows begin with the exact `[M-` glyph triplet.

    Templates come from the ONE shipped glyph renderer (``make_webapp_proof``),
    so a marker row is recognised from pixels alone - no second OCR model and
    no trust in a transcript that could disagree with the published screen.
    """
    import numpy as np

    arr = visual.array(bmp)
    bg = visual.bg_color(arr)
    templates = [np.asarray(wp.glyph(ch, CELL_W, CELL_H), dtype=bool)
                 for ch in MARKER_PREFIX]
    flags: List[bool] = []
    for row in range(ROWS):
        y = GRID_Y + row * CELL_H
        if y + CELL_H > bmp.height:
            break
        matched = True
        for col, template in enumerate(templates):
            x = GRID_X + col * CELL_W
            if x + CELL_W > bmp.width:
                matched = False
                break
            ink = visual.nonbg_mask(arr[y:y + CELL_H, x:x + CELL_W], bg)
            covered = float((ink & template).sum()) / max(1, int(template.sum()))
            stray = int((ink & ~template).sum())
            if covered < 0.70 or stray > 6:
                matched = False
                break
        flags.append(matched)
    return flags


def marker_ink_share(bmp: shot.Bmp) -> float:
    """Share of grid ink that sits in `[M-...]` transcript rows."""
    import numpy as np

    arr = visual.array(bmp)
    mask = visual.nonbg_mask(arr, visual.bg_color(arr))
    flags = marker_row_flags(bmp)
    total = marker = 0
    for row, is_marker in enumerate(flags):
        y = GRID_Y + row * CELL_H
        ink = int(mask[y:y + CELL_H, GRID_X:GRID_X + COLS * CELL_W].sum())
        total += ink
        if is_marker:
            marker += ink
    return marker / max(1, total)


def signature(bmp: shot.Bmp) -> bytes:
    """One 24x24 grayscale signature, derived from Bmp.to_image()."""
    from PIL import Image

    small = bmp.to_image().resize((24, 24), Image.BOX)
    return small.convert("L").tobytes()


def sig_hex(sig: bytes) -> str:
    return hashlib.sha256(sig).hexdigest()


def sig_distance(a: bytes, b: bytes) -> int:
    return sum(abs(x - y) for x, y in zip(a, b))


def analyse(bmp: shot.Bmp, *, framed: bool = True) -> List[str]:
    """Return every visual defect found in one frame ([] means clean)."""
    fails: List[str] = []
    if (bmp.width, bmp.height) != (PANEL_W, PANEL_H):
        fails.append("size is %dx%d, expected %dx%d"
                     % (bmp.width, bmp.height, PANEL_W, PANEL_H))
        return fails

    share = ink_palette_share(bmp)
    if share < PALETTE_INK_MIN:
        fails.append("ink palette share %.3f < %.2f" % (share, PALETTE_INK_MIN))

    hits = theme_guard.scan_bmp(bmp)
    if hits:
        fails.append("default-theme blue present: %s" % theme_guard.describe(hits))

    top, grid, foot = ink_bands(bmp)
    if top < INK_TOP_MIN or grid < INK_GRID_MIN or foot < INK_FOOTER_MIN:
        fails.append("ink bands top=%d grid=%d footer=%d (min %d/%d/%d)"
                     % (top, grid, foot, INK_TOP_MIN, INK_GRID_MIN,
                        INK_FOOTER_MIN))

    tofu = empty_box_cells(bmp)
    if tofu > TOFU_MAX:
        fails.append("empty-box glyphs: %d cells (max %d)" % (tofu, TOFU_MAX))

    share = marker_ink_share(bmp)
    if share > MARKER_INK_MAX:
        fails.append("transcript-marker dump: %.0f%% of grid ink is %s rows "
                     "(max %.0f%%)" % (share * 100, MARKER_PREFIX,
                                       MARKER_INK_MAX * 100))

    if framed:
        hmax, hrows, vmax, vcols = frame_runs(bmp)
        if hmax < H_RUN_MIN or hrows < H_ROWS_MIN:
            fails.append("box-frame horizontal continuity max=%d rows=%d "
                         "(min %d/%d)" % (hmax, hrows, H_RUN_MIN, H_ROWS_MIN))
        if vmax < V_RUN_MIN or vcols < V_COLS_MIN:
            fails.append("box-frame vertical continuity max=%d cols=%d "
                         "(min %d/%d)" % (vmax, vcols, V_RUN_MIN, V_COLS_MIN))
    return fails


# ---------------------------------------------------------------------------
# Gate driver.
# ---------------------------------------------------------------------------
def proof_paths() -> List[Tuple[Path, bool]]:
    paths = [(p, True) for p in sorted(WEB.glob("*.png"))]
    for path in APP_PROOFS:
        if not path.is_file():
            raise GateError("committed app proof is missing: %s" % path)
        paths.append((path, False))
    if len(paths) < 2:
        raise GateError("fewer than two proof PNGs: cannot judge distinctness")
    return paths


def check_signature_table(sigs: Dict[str, bytes]) -> List[str]:
    fails: List[str] = []
    for name, sig in sorted(sigs.items()):
        want = COMMITTED_SIGNATURES.get(name)
        if want is None:
            fails.append("%s has no committed signature (run --update)" % name)
        elif sig_hex(sig) != want:
            fails.append("%s signature changed: got %s committed %s"
                         % (name, sig_hex(sig)[:12], want[:12]))
    for name in sorted(set(COMMITTED_SIGNATURES) - set(sigs)):
        fails.append("committed signature for %s has no proof file" % name)
    return fails


def check_distinct(sigs: Dict[str, bytes]) -> List[str]:
    fails: List[str] = []
    names = sorted(sigs)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            dist = sig_distance(sigs[a], sigs[b])
            if dist <= DUP_L1:
                fails.append("%s and %s are the same screen (L1=%d)"
                             % (a, b, dist))
    return fails


def check_mirrors() -> List[str]:
    fails: List[str] = []
    for path in sorted(MIRROR.glob("*.png")):
        source = WEB / path.name
        if not source.is_file():
            fails.append("docs mirror %s has no source in screenshots/web" % path.name)
        elif path.read_bytes() != source.read_bytes():
            fails.append("docs mirror %s is not byte-identical to the proof" % path.name)
    for path in sorted(WEB.glob("*.png")):
        if not (MIRROR / path.name).is_file():
            fails.append("proof %s has no docs mirror" % path.name)
    return fails


def check() -> int:
    sigs: Dict[str, bytes] = {}
    defects: List[str] = []
    for path, framed in proof_paths():
        bmp = load_bmp(path)
        fails = analyse(bmp, framed=framed)
        for item in fails:
            defects.append("%s: %s" % (path.name, item))
        print("  [%s] %-22s %d visual check(s)"
              % ("PASS" if not fails else "FAIL", path.name,
                 len(fails) if fails else 9 if framed else 6))
        sigs[path.name] = signature(bmp)
    for item in (check_signature_table(sigs) + check_distinct(sigs) +
                 check_mirrors()):
        defects.append(item)
    for item in defects:
        print("  FAIL " + item, file=sys.stderr)
    print("visual proof gate CHECK: %d proof(s), %d defect(s) -> %s"
          % (len(sigs), len(defects), "PASS" if not defects else "FAIL"))
    return 1 if defects else 0


def update() -> int:
    """Re-sign only frames that already pass every visual assertion."""
    sigs: Dict[str, bytes] = {}
    for path, framed in proof_paths():
        bmp = load_bmp(path)
        fails = analyse(bmp, framed=framed)
        if fails:
            print("  REFUSING %s: %s" % (path.name, "; ".join(fails)),
                  file=sys.stderr)
            return 1
        sigs[path.name] = signature(bmp)
    for item in check_distinct(sigs):
        print("  REFUSING duplicate: " + item, file=sys.stderr)
        return 1
    body = "COMMITTED_SIGNATURES: Dict[str, str] = {\n" + "".join(
        '    "%s": "%s",\n' % (name, sig_hex(sig))
        for name, sig in sorted(sigs.items())) + "}\n"
    path = Path(__file__).resolve()
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(r"COMMITTED_SIGNATURES: Dict\[str, str\] = \{.*?\n\}\n",
                         re.DOTALL)
    if not pattern.search(text):
        raise GateError("cannot locate the committed signature block in %s" % path)
    path.write_text(pattern.sub(body, text, count=1), encoding="utf-8", newline="\n")
    print("visual proof gate UPDATE: signed %d proof(s)" % len(sigs))
    return 0


# ---------------------------------------------------------------------------
# Red canaries: a lax gate is a defect, not a pass. Nothing on disk is touched.
# ---------------------------------------------------------------------------
def _synthetic(kind: str):
    """Build a small, deterministic frame with PIL; analyse it via Bmp."""
    from PIL import Image, ImageDraw

    img = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    draw = ImageDraw.Draw(img)
    yellow = _palette()[12]   # BRIGHT_YELLOW from the shipped DOS palette
    # top title rule and bottom status rule, drawn as CP437-style cell strokes
    for row, y in enumerate((GRID_Y, GRID_Y + CELL_H, FOOTER_Y - CELL_H)):
        for col in range(2, COLS - 2):
            x = GRID_X + col * CELL_W
            draw.rectangle((x + 1, y + 8, x + CELL_W - 2, y + 11), fill=yellow)
    for col in (2, COLS - 3):
        x = GRID_X + col * CELL_W
        for row in range(2, ROWS - 2):
            y = GRID_Y + row * CELL_H
            draw.rectangle((x + 5, y + 2, x + 7, y + CELL_H - 3), fill=yellow)
    # body ink, header ink and footer caption ink
    for row in range(4, ROWS - 4, 2):
        for col in range(4, COLS - 4, 2):
            x = GRID_X + col * CELL_W
            y = GRID_Y + row * CELL_H
            draw.rectangle((x + 2, y + 3, x + 8, y + 5),
                           fill=(97, 214, 214))
    draw.rectangle((16, 10, 400, 24), fill=(249, 241, 165))
    draw.rectangle((16, FOOTER_Y + 10, 500, FOOTER_Y + 22), fill=(204, 204, 204))

    if kind == "blank":
        img = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    elif kind == "shell_blue":
        draw.rectangle((10, 340, 420, 390), fill=(0x21, 0x96, 0xF3))
    elif kind == "wrong_palette":
        px = img.load()
        for y in range(PANEL_H):
            for x in range(PANEL_W):
                if px[x, y] != wp.BG:
                    px[x, y] = (0, 255, 0)   # not in the shipped DOS palette
    elif kind == "broken_border":
        # Punch every third cell out of EVERY rule and both side borders so no
        # long contiguous run survives; a single gap would still be continuous
        # through the other rules and would not exercise the detector.
        for y in (GRID_Y, GRID_Y + CELL_H, FOOTER_Y - CELL_H):
            for col in range(2, COLS - 2, 3):
                x = GRID_X + col * CELL_W
                draw.rectangle((x, y + 7, x + CELL_W - 1, y + 12), fill=wp.BG)
        for col in (2, COLS - 3):
            x = GRID_X + col * CELL_W
            for row in range(2, ROWS - 2, 3):
                y = GRID_Y + row * CELL_H
                draw.rectangle((x + 4, y, x + 8, y + CELL_H - 1), fill=wp.BG)
    elif kind == "tofu":
        for col, row in ((10, 8), (12, 8), (14, 8), (16, 8)):
            x, y = GRID_X + col * CELL_W, GRID_Y + row * CELL_H
            draw.rectangle((x + 1, y + 2, x + CELL_W - 2, y + CELL_H - 3),
                           outline=yellow, width=2)

    buf = io.BytesIO()
    img.save(buf, format="BMP")
    return shot.parse_bmp(buf.getvalue())


def _synthetic_markers(n_marker: int, n_plain: int):
    """A deterministic frame whose ink is transcript rows vs plain rows."""
    from PIL import Image

    img = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    colour = _palette()[14]      # BRIGHT_CYAN, a shipped DOS palette colour
    marker = "[M-DOS-CANARY] screen=CANARY panels=3 head=C theme=ice"
    plain = "PANEL DATA 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    for index in range(n_marker):
        y = GRID_Y + index * CELL_H
        for col, ch in enumerate(marker[:COLS]):
            wp.paste_cell(img, ch, colour, GRID_X + col * CELL_W, y)
    for index in range(n_marker, n_marker + n_plain):
        y = GRID_Y + index * CELL_H
        for col, ch in enumerate(plain[:COLS]):
            wp.paste_cell(img, ch, colour, GRID_X + col * CELL_W, y)
    buf = io.BytesIO()
    img.save(buf, format="BMP")
    return shot.parse_bmp(buf.getvalue())


def self_test() -> int:
    checks: List[Tuple[str, bool]] = []
    good = _synthetic("good")
    good_fails = analyse(good, framed=True)
    checks.append(("clean DOS frame accepted", not good_fails))
    if good_fails:
        print("  good frame unexpectedly failed: %s" % "; ".join(good_fails))

    cases = (
        ("blank frame rejected", "blank", ("ink bands",)),
        ("shell chrome / default blue rejected", "shell_blue",
         ("default-theme blue",)),
        ("wrong palette rejected", "wrong_palette", ("ink palette share",)),
        ("broken box border rejected", "broken_border",
         ("horizontal continuity", "vertical continuity")),
        ("empty-box glyphs rejected", "tofu", ("empty-box",)),
    )
    for label, kind, needles in cases:
        fails = analyse(_synthetic(kind), framed=True)
        ok = any(any(needle in item for needle in needles) for item in fails)
        checks.append((label, ok))

    # Transcript-dump regression: a screen whose ink is mostly [M-...] rows
    # must go red, while a normal framed dashboard stays green.
    dumped = analyse(_synthetic_markers(16, 4), framed=False)
    checks.append(("transcript-marker dump rejected",
                   any("transcript-marker dump" in item for item in dumped)))
    framed_ink = analyse(_synthetic_markers(4, 12), framed=False)
    checks.append(("normal text ink is not a marker dump",
                   not any("transcript-marker dump" in item
                           for item in framed_ink)))

    same = signature(good)
    dups = check_distinct({"portal.png": same, "dashboard.png": same})
    checks.append(("duplicate page rejected", bool(dups)))

    for label, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", label))
    ok = bool(checks) and all(item[1] for item in checks)
    print("visual proof gate self-test: %s (%d canaries)"
          % ("PASS" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify committed proofs (default)")
    mode.add_argument("--update", action="store_true", help="re-sign passing proofs")
    mode.add_argument("--self-test", action="store_true", help="red canaries, no fixtures")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        if args.update:
            return update()
        return check()
    except GateError as exc:
        print("visual proof gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
