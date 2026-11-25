#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_webapp_proof.py - honest 1024x600 HOST proof PNGs for the web lane.

PROVENANCE - READ THIS BEFORE QUOTING THE IMAGES
------------------------------------------------
These PNGs are **not device captures**. No board was attached. They visualise
the *actual stdout* of ``test/host/build-wsl/host_webapp_render.elf``, the
standalone harness that drives the firmware's ONE DOM/CSS/canvas core
(``components/markdown`` + ``components/js_canvas``) over committed fixtures.
Nothing is reimplemented here:

  * the text rows and ``NNN@start-end:fg`` colour runs come straight from
    ``dom_render_tree()`` via the harness's ``put_cell`` sink;
  * the fg indices are resolved through the shipped 16-colour DOS palette read
    from ``p4minishell_config.h``;
  * each glyph is rasterised with the shipped unscii-8 TTF using the *same*
    12x20 cell recipe as ``tools/gen_tui_font.py``;
  * the DOS-browser chrome (title bar, address bar, framed content, status
    line and honest footer) is the shared builder in
    ``tools/make_web_screenshots.py`` - no second presentation path.

``docs/assets/web-features.png`` shows the dashboard fixture (CSS grid, table
and canvas) and ``docs/assets/webapp-forms.png`` is a 2x detail crop of the SAME
host render of the form fixture (inputs, select, submit handler). Both captions
say HOST PROOF - NOT A DEVICE CAPTURE. A pass is a host pre-gate, never
hardware sign-off.

Usage
-----
    python tools/make_webapp_proof.py --check      # committed PNGs == fresh render?
    python tools/make_webapp_proof.py --update     # (re)write both PNGs
    python tools/make_webapp_proof.py --self-test  # red canaries: prove --check can fail
"""
from __future__ import annotations

import hashlib
import importlib.util
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
ASSETS = ROOT / "docs" / "assets"
FIXTURES = TOOLS / "host" / "fixtures" / "webapp"
BASELINE = FIXTURES / "baseline"
TTF = (ROOT / "managed_components" / "lvgl__lvgl" / "scripts" / "generators" /
       "built_in_font" / "unscii-8.ttf")

FIXTURE = "dashboard.html"
FORM_FIXTURE = "form.html"
OUT_FULL = ASSETS / "web-features.png"
OUT_FORMS = ASSETS / "webapp-forms.png"

PANEL_W, PANEL_H = 1024, 600
CELL_W, CELL_H = 12, 20          # the device's primary 80x25 TUI cell
COLS, ROWS = 80, 25
GRID_X, GRID_Y = 32, 46          # 960x500 grid, centred; header/footer remain
FOOTER_Y = GRID_Y + ROWS * CELL_H
SCALE = 2                        # detail crop magnification
DETAIL_ROWS = (5, 14)            # 1-based inclusive: the form panel
DETAIL_COLS = (1, 40)

BG = (12, 12, 12)                # P4_CONFIG_ANSI_BLACK
BORDER = (90, 76, 40)
WARN_FG = (249, 241, 165)

PALETTE_NAMES = [
    "BLACK", "RED", "GREEN", "YELLOW", "BLUE", "MAGENTA", "CYAN", "WHITE",
    "BRIGHT_BLACK", "BRIGHT_RED", "BRIGHT_GREEN", "BRIGHT_YELLOW",
    "BRIGHT_BLUE", "BRIGHT_MAGENTA", "BRIGHT_CYAN", "BRIGHT_WHITE",
]

# Text that must actually appear in the gate-validated harness emission.
EXPECTED_TEXT = ("LIVE DASHBOARD", "UPTIME 99.7%", "TRAFFIC", "NODE")
FORM_EXPECTED_TEXT = ("ACCOUNT SETTINGS", "PROFILE", "[Ada]", "[ Save ]")


# --------------------------------------------------------------------------
# Cell rasterisation - same unscii-8 12x20 recipe as tools/gen_tui_font.py.
# --------------------------------------------------------------------------
_GLYPH_CACHE: Dict[Tuple[str, int, int], Image.Image] = {}


def glyph(char: str, w: int, h: int) -> Image.Image:
    key = (char, w, h)
    if key in _GLYPH_CACHE:
        return _GLYPH_CACHE[key]
    if not TTF.is_file():
        raise RuntimeError("shipped unscii-8 TTF is missing: %s" % TTF)
    font = ImageFont.truetype(str(TTF), w)
    im = Image.new("L", (w, w), 0)
    ImageDraw.Draw(im).text((0, 0), char, fill=255, font=font)
    if (w, w) != (w, h):
        im = im.resize((w, h), Image.LANCZOS)
    mask = im.point(lambda v: 255 if v > 127 else 0)
    _GLYPH_CACHE[key] = mask
    return mask


def paste_cell(panel: Image.Image, char: str, colour: Tuple[int, int, int],
               x: int, y: int, w: int = CELL_W, h: int = CELL_H) -> None:
    if char == " " or ord(char) < 0x20:
        return
    panel.paste(Image.new("RGB", (w, h), colour), (x, y), glyph(char, w, h))


# --------------------------------------------------------------------------
# The ONE existing gate and renderer - imported, never copied.
# --------------------------------------------------------------------------
def load_gate():
    path = TOOLS / "host_webapp_gate.py"
    spec = importlib.util.spec_from_file_location("host_webapp_gate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the existing host webapp gate")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses looks the module up here
    spec.loader.exec_module(module)
    return module


def load_palette() -> List[Tuple[int, int, int]]:
    """Read the shipped DOS palette; refuse to guess if the header changed."""
    text = (ROOT / "p4minishell_config.h").read_text(encoding="utf-8")
    found = {}
    for name, value in re.findall(
            r"#define\s+P4_CONFIG_ANSI_([A-Z0-9_]+)\s+0x([0-9A-Fa-f]{6})", text):
        found[name] = tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))
    missing = [n for n in PALETTE_NAMES if n not in found]
    if missing:
        raise RuntimeError("p4minishell_config.h lacks ANSI palette entries: %s"
                           % ", ".join(missing))
    return [found[n] for n in PALETTE_NAMES]


def emit_raw(fixture: str = FIXTURE):
    """Run the real harness and validate its report against its baseline."""
    gate = load_gate()
    renderer = gate.default_renderer()
    raw = gate.run_renderer(renderer, FIXTURES / fixture, FIXTURES)
    gate.evaluate(raw, fixture, FIXTURES, BASELINE, update=False,
                  compare_frames=False)   # raises on a stale/vacuous report
    return raw, gate


def parse_grid(result) -> Tuple[Dict[int, str], Dict[int, List[int]]]:
    """Recover per-cell glyph and foreground from the gate-validated dump."""
    texts = dict(result.rows)
    fgs: Dict[int, List[int]] = {}
    for row, runs in result.runs.items():
        fg = [7] * COLS
        for item in runs:
            span, colour = item.rsplit(":", 1)
            start, end = (int(v) for v in span.split("-", 1))
            for col in range(start - 1, min(end, COLS)):
                index = int(colour)
                # 16 is the renderer's "no explicit foreground" marker after a
                # body background paint; fall back to the DOS light grey.
                fg[col] = 7 if index >= 16 else index
        fgs[row] = fg
    if not texts or not fgs:
        raise RuntimeError("harness emitted no rows/colour runs")
    return texts, fgs


def _shared():
    """Import the shared screenshot chrome lazily (it imports this module)."""
    sys.path.insert(0, str(TOOLS))
    import make_web_screenshots as mws  # noqa: E402
    return mws


def _case(fixture: str):
    mws = _shared()
    name, title, address, status, words, prefixes = mws.CASES[fixture]
    return mws, title, address, status, words, prefixes


def metadata_lines(raw: str) -> List[str]:
    return [line.strip() for line in raw.replace("\r\n", "\n").split("\n")
            if line.startswith(("webapp.meta ", "webapp.css ",
                                "webapp.events ", "webapp.canvas "))]


def build_detail(texts: Dict[int, str], fgs: Dict[int, List[int]],
                 palette: Sequence[Tuple[int, int, int]], mws,
                 title: str, address: str, status: str) -> Image.Image:
    panel = Image.new("RGB", (PANEL_W, PANEL_H), BG)
    mws.chrome(panel, title, address, status,
               "FORM DETAIL - 2x crop of the SAME host render")
    r0, r1 = DETAIL_ROWS
    c0, c1 = DETAIL_COLS
    crop_w = (c1 - c0 + 1) * CELL_W
    crop_h = (r1 - r0 + 1) * CELL_H
    crop = Image.new("RGB", (crop_w, crop_h), BG)
    for row in range(r0, r1 + 1):
        line = texts.get(row, "")
        fg = fgs.get(row, [7] * COLS)
        for col in range(c0 - 1, c1):
            char = line[col] if col < len(line) else " "
            mws.wp.paste_cell(crop, char, palette[fg[col]],
                              (col - c0 + 1) * CELL_W, (row - r0) * CELL_H)
    big = crop.resize((crop_w * SCALE, crop_h * SCALE), Image.NEAREST)
    ox = (PANEL_W - big.width) // 2
    oy = GRID_Y + (FOOTER_Y - GRID_Y - big.height) // 2
    ImageDraw.Draw(panel).rectangle((ox - 4, oy - 4, ox + big.width + 3,
                                     oy + big.height + 3), outline=BORDER)
    panel.paste(big, (ox, oy))
    return panel


# --------------------------------------------------------------------------
# Verification (the ONE pixel model is used for the theme scan).
# --------------------------------------------------------------------------
def verify(img: Image.Image, expected: Image.Image, texts: Dict[int, str],
           metadata: Sequence[str], label: str,
           expected_text: Sequence[str] = EXPECTED_TEXT,
           prefixes: Sequence[str] = ("webapp.meta ",),
           detail: bool = False) -> Dict[str, bool]:
    out: Dict[str, bool] = {}
    out["size_" + label] = img.size == (PANEL_W, PANEL_H) and img.mode == "RGB"
    if not out["size_" + label]:
        return out
    out["deterministic_" + label] = img.tobytes() == expected.tobytes()
    joined = "\n".join(texts.values())
    out["real_dom_text_" + label] = all(w in joined for w in expected_text)
    out["feature_report_" + label] = all(
        any(m.startswith(p) for m in metadata) for p in prefixes)
    sys.path.insert(0, str(TOOLS))
    from p4test import theme_guard  # noqa: E402  (the shipped pixel model)
    out["no_default_theme_" + label] = not theme_guard.scan_image(img)
    px = img.load()
    ink = sum(1 for y in range(GRID_Y, FOOTER_Y, 3)
              for x in range(GRID_X, GRID_X + COLS * CELL_W, 3)
              if sum(px[x, y]) > 240)
    out["grid_ink_" + label] = ink >= 40
    out.update(_shared().text_checks(texts, label))
    return out


def report(results: Dict[str, bool]) -> None:
    for key in sorted(results):
        print("  [%s] %s" % ("PASS" if results[key] else "FAIL", key))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fresh() -> Tuple[Image.Image, Image.Image, Dict[int, str], Dict[int, str],
                     List[str], List[str], Sequence[str]]:
    raw, _ = emit_raw(FIXTURE)
    full_texts, full_fgs = parse_grid(
        load_gate().parse_emission(raw, FIXTURE, FIXTURES))
    _, _, _, _, _, full_prefixes = _case(FIXTURE)
    mws, title, address, status, _, _ = _case(FIXTURE)
    full = mws.build_full(full_texts, full_fgs, load_palette(), title,
                          address, status,
                          "DASHBOARD FEATURE PROOF - grid, table & canvas")

    form_raw, _ = emit_raw(FORM_FIXTURE)
    form_texts, form_fgs = parse_grid(
        load_gate().parse_emission(form_raw, FORM_FIXTURE, FIXTURES))
    mws, title, address, status, _, _ = _case(FORM_FIXTURE)
    detail = build_detail(form_texts, form_fgs, load_palette(), mws,
                          title, address, status)
    return (full, detail, full_texts, form_texts,
            metadata_lines(raw), metadata_lines(form_raw), full_prefixes)


def update() -> int:
    full, detail, _, _, _, _, _ = fresh()
    for path, image in ((OUT_FULL, full), (OUT_FORMS, detail)):
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path)
        print("wrote %s (%dx%d, host-generated proof) sha256=%s"
              % (path.relative_to(ROOT), *image.size, sha256(path)))
    return 0


def check() -> int:
    full, detail, full_texts, form_texts, full_meta, form_meta, prefixes = fresh()
    ok = True
    cases = (
        (OUT_FULL, full, "web-features", full_texts, full_meta,
         EXPECTED_TEXT, prefixes),
        (OUT_FORMS, detail, "webapp-forms", form_texts, form_meta,
         FORM_EXPECTED_TEXT, ("webapp.meta ", "webapp.events ")),
    )
    for path, expected, label, texts, meta, words, pres in cases:
        if not path.is_file():
            print("check: MISSING %s (run --update)" % path.relative_to(ROOT))
            return 1
        with Image.open(path) as handle:
            img = handle.convert("RGB")
        results = verify(img, expected, texts, meta, label, words, pres)
        report(results)
        good = all(results.values())
        ok = ok and good
        print("check %s: %s sha256=%s (host proof; not a device capture)"
              % (label, "PASS" if good else "FAIL", sha256(path)))
    return 0 if ok else 1


def self_test() -> int:
    """RED CANARY: every guard must be seen failing on a concrete bad image."""
    full, detail, texts, form_texts, metadata, form_meta, prefixes = fresh()
    good = verify(full, full, texts, metadata, "web-features",
                  EXPECTED_TEXT, prefixes)
    green = all(good.values())
    print("self-test honest render: %s" % ("PASS" if green else "FAIL"))

    tampered = full.copy()
    tampered.putpixel((GRID_X + 4, GRID_Y + 4), (255, 0, 255))
    bad_hash = not verify(tampered, full, texts, metadata, "web-features",
                          EXPECTED_TEXT, prefixes)["deterministic_web-features"]

    blanked = Image.new("RGB", (PANEL_W, PANEL_H), BG)
    bad_ink = not verify(blanked, blanked, texts, metadata, "web-features",
                         EXPECTED_TEXT, prefixes)["grid_ink_web-features"]

    blue = full.copy()
    ImageDraw.Draw(blue).rectangle((400, 560, 600, 590), fill=(0x21, 0x96, 0xF3))
    bad_theme = not verify(blue, blue, texts, metadata, "web-features",
                           EXPECTED_TEXT, prefixes)["no_default_theme_web-features"]

    stale_meta = [m for m in metadata if not m.startswith("webapp.meta")]
    bad_report = not verify(full, full, texts, stale_meta, "web-features",
                            EXPECTED_TEXT, prefixes)["feature_report_web-features"]

    checks = {
        "green passes": green,
        "red: tampered pixel rejected": bad_hash,
        "red: blank grid rejected": bad_ink,
        "red: LVGL default-blue blob rejected": bad_theme,
        "red: stale feature report rejected": bad_report,
        "detail crop differs from full page": full.tobytes() != detail.tobytes(),
    }
    for name, ok in checks.items():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = all(checks.values())
    print("make_webapp_proof self-test: %s" % ("PASS" if ok else "FAIL"))
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
        print("make_webapp_proof: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
