#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_web_screenshots.py - honest 1024x600 HOST proof PNGs for the web lane.

PROVENANCE - READ THIS BEFORE QUOTING THE IMAGES
------------------------------------------------
These PNGs are **not device captures**. No board was attached. Every content
cell comes from the *actual stdout* of the shipped
``test/host/build-wsl/host_webapp_render.elf`` harness: the firmware's ONE
``dom_parse`` / CSS cascade / ``dom_render_tree`` path over the committed
functional fixtures in ``tools/host/fixtures/webapp``. The harness output is
first checked against its committed baseline by ``tools/host_webapp_gate.py``
(so a stale or vacuous report cannot be published), then each glyph is drawn
with the same unscii-8 12x20 cell recipe as ``tools/gen_tui_font.py`` and the
shipped DOS palette from ``p4minishell_config.h``.

The surrounding chrome is a DOS-browser presentation of that same 80x25 grid:
a CP437 title bar, a boxed address bar, a framed content area and a status
line, all using the shipped font and palette. PIL assembles cells and writes
the explicit HOST-PROOF footer; it does not reimplement DOM, CSS, entity,
colour or pixel models. A pass is a host pre-gate only, never hardware
sign-off (harness.md 2.2).

Usage
-----
    python tools/make_web_screenshots.py --check      # committed PNGs == fresh render?
    python tools/make_web_screenshots.py --update     # (re)write the four PNGs
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
DOCS_WEB = ROOT / "docs" / "assets" / "web"

# The ONE existing proof builder / gate / pixel model are imported, never copied.
sys.path.insert(0, str(TOOLS))
import make_webapp_proof as wp  # noqa: E402

PANEL_W, PANEL_H = wp.PANEL_W, wp.PANEL_H
CELL_W, CELL_H = wp.CELL_W, wp.CELL_H
COLS, ROWS = wp.COLS, wp.ROWS
GRID_X, GRID_Y = wp.GRID_X, wp.GRID_Y
FOOTER_Y = GRID_Y + ROWS * CELL_H

# The Wikipedia proof is a board-free pre-gate: only the first 25 rows render
# and the host probe has no put_link sink. Say so on the proof itself.
CAPTIONS = {
    "wiki_esp32.png":
        "WIKI_ESP32 | first 25 rows, no tap links on host",
}

# fixture -> (output, title bar, address bar, status token, expected text,
#             feature-report prefixes)
CASES = {
    "portal.html": ("portal.png", "P4.NET/PORTAL.80COL",
                    "http://p4.local/portal.html",
                    "READY | 3 links | press F1 for help",
                    ("WELCOME TO P4 NET", "MAIN MENU", "(C) 2026 P4 NET",
                     "Dash", "Form"),
                    ("webapp.meta ",)),
    "dashboard.html": ("dashboard.png", "P4.NET/DASHBOARD.80COL",
                       "http://p4.local/dashboard.html",
                       "READY | panels + table + chart | F1 help",
                       ("LIVE DASHBOARD", "UPTIME 99.7%", "TRAFFIC",
                        "NODE", "LAST 24 HOURS"),
                       ("webapp.meta ", "webapp.canvas ")),
    "article.html": ("article.png", "P4.NET/ARTICLE.80COL",
                     "http://p4.local/article.html",
                     "READY | 2-column reading view | F1 help",
                     ("THE DOS BROWSER REVISITED", "-- em dash",
                      "21degC", "(tm) mark", "Columns keep"),
                     ("webapp.meta ",)),
    "form.html": ("form.png", "P4.NET/FORM.80COL",
                  "http://p4.local/form.html",
                  "READY | submit saves settings | F1 help",
                  ("ACCOUNT SETTINGS", "PROFILE", "[Ada]", "[Pro]",
                   "[x]", "[ Save ]"),
                  ("webapp.meta ", "webapp.events ")),
    "wiki_esp32.html": ("wiki_esp32.png", "P4.NET/WIKI.80COL",
                        "http://p4.local/wiki_esp32.html",
                        "READY | first 25 rows of the article | F1 help",
                        ("ESP32", "Espressif Systems", "240 MHz",
                         "official ESP-IDF page"),
                        ("webapp.meta ", "webapp.image ")),
}

NOTICE = "HOST PROOF - NOT A DEVICE CAPTURE"
BAR_BG = (24, 20, 14)          # warm DOS chrome strip, not a new palette


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


def _bar_row(panel: Image.Image, y: int, text: str,
             colour: Tuple[int, int, int]) -> None:
    """Paste an 80-cell CP437 row with the shipped glyph renderer."""
    text = text[:COLS].ljust(COLS)
    ImageDraw.Draw(panel).rectangle((GRID_X - 8, y - 2,
                                     GRID_X + COLS * CELL_W + 7,
                                     y + CELL_H + 1), fill=BAR_BG)
    for col, char in enumerate(text):
        wp.paste_cell(panel, char, colour,
                      GRID_X + col * CELL_W, y)


def chrome(panel: Image.Image, title: str, address: str, status: str,
           caption: str) -> None:
    """A DOS browser frame around the gate-validated 80x25 content grid."""
    yellow = wp.load_palette()[14]
    cyan = wp.load_palette()[11]

    # One window frame above the grid. The fixture itself supplies the page's
    # title bar, address bar and status line in the gate-validated 80x25 rows,
    # so the chrome must not paint a second copy of them.
    window = "P4 MINI SHELL - DOS BROWSER"
    top = "┌─[" + window + "]" + "─" * (COLS - len(window) - 5) + "┐"
    _bar_row(panel, 6, top, yellow)

    # Framed content area: CP437 corners and vertical rails beside the grid.
    draw = ImageDraw.Draw(panel)
    draw.rectangle((GRID_X - 9, GRID_Y - 3,
                    GRID_X + COLS * CELL_W + 8, FOOTER_Y + 2),
                   outline=wp.BORDER)
    for row in range(ROWS):
        y = GRID_Y + row * CELL_H
        wp.paste_cell(panel, "│", yellow, GRID_X - 8, y)
        wp.paste_cell(panel, "│", yellow, GRID_X + COLS * CELL_W, y)

    # A bottom rule closes the framed content area; the page's own status
    # line is already drawn by the renderer inside the grid.
    _bar_row(panel, FOOTER_Y + 8,
             "└" + "─" * (COLS - 2) + "┘", wp.BORDER)

    notice_font = ImageFont.truetype(str(wp.TTF), 11)
    caption_font = ImageFont.truetype(str(wp.TTF), 12)
    draw.text((16, FOOTER_Y + 43), caption, fill=cyan, font=caption_font)
    width = int(draw.textlength(NOTICE, font=notice_font))
    draw.text((PANEL_W - width - 16, FOOTER_Y + 43), NOTICE,
              fill=wp.WARN_FG, font=notice_font)


def build_full(texts: Dict[int, str], fgs: Dict[int, List[int]],
               palette: Sequence[Tuple[int, int, int]], title: str,
               address: str, status: str, caption: str) -> Image.Image:
    panel = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    chrome(panel, title, address, status, caption)
    for row in range(1, ROWS + 1):
        line = texts.get(row, "")
        fg = fgs.get(row, [7] * COLS)
        for col, char in enumerate(line[:COLS]):
            wp.paste_cell(panel, char, palette[fg[col]],
                          GRID_X + col * CELL_W,
                          GRID_Y + (row - 1) * CELL_H)
    return panel


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata_lines(raw: str) -> List[str]:
    return [line.strip() for line in raw.replace("\r\n", "\n").split("\n")
            if line.startswith(("webapp.entity ", "webapp.cascade ",
                                "webapp.meta ", "webapp.css ",
                                "webapp.events ", "webapp.canvas ",
                                "webapp.image "))]


def text_checks(texts: Dict[int, str], label: str,
                framed: bool = True) -> Dict[str, bool]:
    """Presentation assertions on the ONE gate-validated text grid."""
    title = texts.get(2, "")
    address = texts.get(3, "")
    bottoms = [row for row in texts.values()
               if "└" in row and "─" in row]
    joined = "\n".join(texts.values())
    return {
        "box_title_" + label: not framed or ("┌" in title and "┐" in title and
                                             title.count("─") >= 20),
        "box_address_" + label: not framed or ("├" in address and "┤" in address
                                               and "Address:" in address),
        "box_rails_" + label: not framed or (
            any("│" in row for row in texts.values()) and bool(bottoms)),
        "no_glyph_boxes_" + label: ("?" not in joined and
                                    "\ufffd" not in joined and
                                    "\u25a1" not in joined and
                                    "\ufffd" not in joined),
    }


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
    # The wiki article has no in-page CP437 frame: the DOS-browser chrome around
    # the grid supplies it. Every showcase fixture keeps its in-page frame.
    out.update(text_checks(texts, label, framed=label != "wiki_esp32"))
    return out


def report(results: Dict[str, bool]) -> None:
    for key in sorted(results):
        print("  [%s] %s" % ("PASS" if results[key] else "FAIL", key))


def fresh_all():
    """Build every proof image plus the facts needed to verify them."""
    palette = wp.load_palette()
    items = []
    for fixture, case in CASES.items():
        name, title, address, status, words, prefixes = case
        raw, texts, fgs = fresh(fixture)
        meta = metadata_lines(raw)
        items.append((name, build_full(texts, fgs, palette, title, address,
                                       status, CAPTIONS.get(
                                           name, Path(name).stem.upper() +
                                           " | real HTML in one DOS renderer")),
                      texts, meta, words, prefixes))
    return items


def update() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    DOCS_WEB.mkdir(parents=True, exist_ok=True)
    for name, image, *_ in fresh_all():
        for directory in (OUT, DOCS_WEB):
            path = directory / name
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
        label = Path(name).stem
        results = verify(img, expected, texts, meta, words, prefixes, label)
        report(results)
        good = all(results.values())
        ok = ok and good
        print("check %s: %s sha256=%s (host proof; not a device capture)"
              % (name, "PASS" if good else "FAIL", sha256(path)))
    return 0 if ok else 1


def self_test() -> int:
    """RED CANARY: every guard must fail on a concrete bad image/report."""
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

    stale_meta = [m for m in meta if not m.startswith("webapp.meta")]
    checks["red: stale feature report rejected"] = not verify(
        good, good, texts, stale_meta, words, prefixes,
        label)["feature_report_" + label]

    no_frame = dict(texts)
    no_frame[2] = "X" * COLS
    checks["red: missing CP437 title frame rejected"] = not text_checks(
        no_frame, label)["box_title_" + label]

    tofu = dict(texts)
    tofu[7] = "\u25a1" + tofu.get(7, "")[1:]
    checks["red: empty-box glyph rejected"] = not text_checks(
        tofu, label)["no_glyph_boxes_" + label]

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
