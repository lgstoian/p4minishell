#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_web_screenshots.py - honest 1024x600 HOST proof PNGs for the web lane.

PROVENANCE - READ THIS BEFORE QUOTING THE IMAGES
------------------------------------------------
These PNGs are **not device captures**. No board was attached. Every pixel of
the 80x25 grid comes from the *actual stdout* of the shipped
``test/host/build-wsl/host_webapp_render.elf`` harness: the firmware's ONE
``dom_parse`` / CSS cascade / ``dom_render_tree`` path over the committed
fixtures in ``tools/host/fixtures/webapp``. The harness output is first checked
against its committed baseline by ``tools/host_webapp_gate.py`` (so a stale or
vacuous report cannot be published), then each glyph is drawn with the same
unscii-8 12x20 cell recipe as ``tools/gen_tui_font.py`` and the shipped DOS
palette from ``p4minishell_config.h``. PIL only assembles cells and draws the
explicit HOST-PROOF chrome; no DOM, CSS, entity, colour or pixel model is
reimplemented here.

A pass is a host pre-gate only. It is never hardware sign-off (harness.md 2.2).

Usage
-----
    python tools/make_web_screenshots.py --check      # committed PNGs == fresh render?
    python tools/make_web_screenshots.py --update     # (re)write the PNGs
    python tools/make_web_screenshots.py --self-test  # red canaries: --check can fail
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
OUT = ROOT / "screenshots" / "web"

# The ONE existing proof builder / gate / pixel model are imported, never copied.
sys.path.insert(0, str(TOOLS))
import make_webapp_proof as wp  # noqa: E402

PANEL_W, PANEL_H = wp.PANEL_W, wp.PANEL_H
CELL_W, CELL_H = wp.CELL_W, wp.CELL_H
COLS, ROWS = wp.COLS, wp.ROWS
GRID_X, GRID_Y = wp.GRID_X, wp.GRID_Y
FOOTER_Y = GRID_Y + ROWS * CELL_H

# fixture -> (output, title, expected text, feature-report prefixes)
CASES = {
    "modern.html": ("modern.png",
                    "MODERN SITE SUBSET - entities + CSS cascade",
                    ("Entities", "Source order", "Eleven classes",
                     "Duplicate attribute", "clamped"),
                    ("webapp.entity ", "webapp.cascade ")),
    "recovery.html": ("recovery.png",
                      "PARSE RECOVERY - implied end tags + truncated source",
                      ("Truncated markup recovers", "First paragraph",
                       "Second paragraph", "Duplicate id keeps first"),
                      ("webapp.meta ",)),
}

DETAIL = ("modern-entities.png", "ENTITIES + CASCADE - 2x detail of modern.html",
          (1, 12), (1, 60))


def load_gate():
    return wp.load_gate()


def fresh(fixture: str):
    """Run the real harness, gate-validate its report, return grid + metadata."""
    gate = load_gate()
    renderer = gate.default_renderer()
    raw = gate.run_renderer(renderer, wp.FIXTURES / fixture, wp.FIXTURES)
    gate.evaluate(raw, fixture, wp.FIXTURES, wp.BASELINE, update=False,
                  compare_frames=False)   # raises on stale/vacuous report
    result = gate.parse_emission(raw, fixture, wp.FIXTURES)
    texts, fgs = wp.parse_grid(result)
    return raw, texts, fgs


def chrome(panel: Image.Image, title: str, footer: str, detail: bool) -> None:
    notice = "HOST PROOF - NOT A DEVICE CAPTURE"
    font = ImageFont.truetype(str(wp.TTF), 11)
    width = int(ImageDraw.Draw(panel).textlength(notice, font=font))
    title_font = ImageFont.truetype(str(wp.TTF), 13)
    wp.draw_label(panel, wp.fit(title, title_font, PANEL_W - width - 48),
                  16, 8, wp.BAR_FG)
    ImageDraw.Draw(panel).text((PANEL_W - width - 16, 10), notice,
                               fill=wp.WARN_FG, font=font)
    ImageDraw.Draw(panel).line((8, FOOTER_Y - 1, PANEL_W - 8, FOOTER_Y - 1),
                               fill=wp.BORDER)
    wp.draw_label(panel, footer, 16, FOOTER_Y + 8, wp.BAR_FG, size=11)
    if detail:
        ImageDraw.Draw(panel).text(
            (16, FOOTER_Y + 28),
            "2x crop of the SAME host render; unscii-8 cells; no board verification",
            fill=(150, 210, 210), font=font)


def build_full(texts: Dict[int, str], fgs: Dict[int, List[int]],
               palette: Sequence[Tuple[int, int, int]], title: str) -> Image.Image:
    panel = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    chrome(panel, title,
           "DOM/CSS/entities via host_webapp_gate.py | shipped palette | "
           "no board verification", False)
    for row in range(1, ROWS + 1):
        line = texts.get(row, "")
        fg = fgs.get(row, [7] * COLS)
        for col, char in enumerate(line[:COLS]):
            wp.paste_cell(panel, char, palette[fg[col]],
                          GRID_X + col * CELL_W, GRID_Y + (row - 1) * CELL_H)
    return panel


def build_detail(texts: Dict[int, str], fgs: Dict[int, List[int]],
                 palette: Sequence[Tuple[int, int, int]], title: str) -> Image.Image:
    panel = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    chrome(panel, title, "2x detail crop of the SAME gate-validated render",
           True)
    r0, r1 = DETAIL[2]
    c0, c1 = DETAIL[3]
    crop_w = (c1 - c0 + 1) * CELL_W
    crop_h = (r1 - r0 + 1) * CELL_H
    crop = Image.new("RGB", (crop_w, crop_h), wp.BG)
    for row in range(r0, r1 + 1):
        line = texts.get(row, "")
        fg = fgs.get(row, [7] * COLS)
        for col in range(c0 - 1, c1):
            char = line[col] if col < len(line) else " "
            wp.paste_cell(crop, char, palette[fg[col]],
                          (col - c0 + 1) * CELL_W, (row - r0) * CELL_H)
    big = crop.resize((crop_w * wp.SCALE, crop_h * wp.SCALE), Image.NEAREST)
    ox = (PANEL_W - big.width) // 2
    oy = GRID_Y + (FOOTER_Y - GRID_Y - big.height) // 2
    ImageDraw.Draw(panel).rectangle((ox - 4, oy - 4, ox + big.width + 3,
                                     oy + big.height + 3), outline=wp.BORDER)
    panel.paste(big, (ox, oy))
    return panel


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata_lines(raw: str) -> List[str]:
    return [line.strip() for line in raw.replace("\r\n", "\n").split("\n")
            if line.startswith(("webapp.entity ", "webapp.cascade ",
                                "webapp.meta ", "webapp.css ",
                                "webapp.events ", "webapp.canvas "))]


def verify(img: Image.Image, expected: Image.Image, texts: Dict[int, str],
           meta: Sequence[str], words: Sequence[str], prefixes: Sequence[str],
           label: str) -> Dict[str, bool]:
    out: Dict[str, bool] = {}
    out["size_" + label] = img.size == (PANEL_W, PANEL_H) and img.mode == "RGB"
    if not out["size_" + label]:
        return out
    out["deterministic_" + label] = img.tobytes() == expected.tobytes()
    joined = "\n".join(texts.values())
    out["real_dom_text_" + label] = all(w in joined for w in words)
    out["feature_report_" + label] = all(
        any(m.startswith(p) for m in meta) for p in prefixes)
    sys.path.insert(0, str(TOOLS))
    from p4test import theme_guard  # noqa: E402 - the shipped pixel model
    out["no_default_theme_" + label] = not theme_guard.scan_image(img)
    px = img.load()
    ink = sum(1 for y in range(GRID_Y, FOOTER_Y, 3)
              for x in range(GRID_X, GRID_X + COLS * CELL_W, 3)
              if sum(px[x, y]) > 240)
    out["grid_ink_" + label] = ink >= 40
    return out


def report(results: Dict[str, bool]) -> None:
    for key in sorted(results):
        print("  [%s] %s" % ("PASS" if results[key] else "FAIL", key))


def fresh_all():
    """Build every proof image plus the facts needed to verify them."""
    palette = wp.load_palette()
    items = []
    for fixture, (name, title, words, prefixes) in CASES.items():
        raw, texts, fgs = fresh(fixture)
        meta = metadata_lines(raw)
        items.append((name, build_full(texts, fgs, palette, title),
                      texts, meta, words, prefixes))
    # The detail crop is derived from the SAME modern.html render.
    raw, texts, fgs = fresh("modern.html")
    items.append((DETAIL[0],
                  build_detail(texts, fgs, palette, DETAIL[1]),
                  texts, metadata_lines(raw), CASES["modern.html"][2],
                  CASES["modern.html"][3]))
    return items


def update() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, image, *_ in fresh_all():
        path = OUT / name
        image.save(path)
        print("wrote %s (%dx%d, host proof) sha256=%s"
              % (path.relative_to(ROOT), *image.size, sha256(path)))
    return 0


def check() -> int:
    ok = True
    for name, expected, texts, meta, words, prefixes in fresh_all():
        path = OUT / name
        if not path.is_file():
            print("check: MISSING %s (run --update)" % path.relative_to(ROOT))
            return 1
        with Image.open(path) as handle:
            img = handle.convert("RGB")
        results = verify(img, expected, texts, meta, words, prefixes, Path(name).stem)
        report(results)
        good = all(results.values())
        ok = ok and good
        print("check %s: %s sha256=%s (host proof; not a device capture)"
              % (name, "PASS" if good else "FAIL", sha256(path)))
    return 0 if ok else 1


def self_test() -> int:
    """RED CANARY: every guard must fail on a concrete bad image."""
    checks: Dict[str, bool] = {}
    name, good, texts, meta, words, prefixes = fresh_all()[0]
    label = Path(name).stem
    green = verify(good, good, texts, meta, words, prefixes, label)
    checks["green passes"] = all(green.values())

    tampered = good.copy()
    tampered.putpixel((GRID_X + 4, GRID_Y + 4), (255, 0, 255))
    checks["red: tampered pixel rejected"] = not verify(
        tampered, good, texts, meta, words, prefixes,
        label)["deterministic_" + label]

    blank = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    checks["red: blank grid rejected"] = not verify(
        blank, blank, texts, meta, words, prefixes, label)["grid_ink_" + label]

    blue = good.copy()
    ImageDraw.Draw(blue).rectangle((400, 560, 600, 590), fill=(0x21, 0x96, 0xF3))
    checks["red: LVGL default-blue blob rejected"] = not verify(
        blue, blue, texts, meta, words, prefixes,
        label)["no_default_theme_" + label]

    stale_meta = [m for m in meta if not m.startswith("webapp.entity")]
    checks["red: stale feature report rejected"] = not verify(
        good, good, texts, stale_meta, words, prefixes,
        label)["feature_report_" + label]

    for key, ok in checks.items():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", key))
    ok = all(checks.values())
    print("make_web_screenshots self-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    try:
        if "--self-test" in args:
            return self_test()
        if "--update" in args:
            return update()
        return check()
    except Exception as exc:  # noqa: BLE001 - refuse to emit an imitation
        print("make_web_screenshots: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
