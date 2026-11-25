#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_app_screenshots.py - honest 1024x600 HOST proof PNGs for app dashboards.

PROVENANCE - READ THIS BEFORE QUOTING THE IMAGES
------------------------------------------------
These PNGs are **not device captures**. No board was attached. Every cell comes
from the *real* 80x25 cell grid the shipped host batch interpreter prints after
it executes the fixture's ``draw`` verbs in ``tools/host/fixtures/apps/<APP>.BAT``
through the ONE pure TUI core (``components/tui/tui_frame.h``). The runner emits
a ``tui dump /a``-style report: normalised cell rows (four ASCII hex digits per
codepoint, so a cp1252 subprocess pipe cannot corrupt them) plus
``NNN@start-end:fg`` colour runs. ``host_tui_gate.parse_emission`` validates
that report, ``host_batch_run.evaluate`` checks the marker transcript against
its committed baseline, and ``host_apps_gate`` checks the app identity and its
uniqueness - so a stale, vacuous, duplicated or hand-drawn dashboard cannot be
published.

Nothing here is a second renderer or a second palette: glyph rasterisation, the
12x20 cell recipe and the one DOS palette come from ``tools/make_webapp_proof.py``;
the visual quality, palette, frame, ink, tofu and theme judgements come from
``tools/visual_proof_gate.py`` (the shared ``p4test`` pixel model). This file
only decodes the gated cell grid into palette-indexed cells and writes the honest
HOST-PROOF footer. A pass is a board-free pre-gate only, never hardware sign-off
(harness.md 2.2).

Usage
-----
    python tools/make_app_screenshots.py --check      # committed PNGs == fresh render?
    python tools/make_app_screenshots.py --update     # (re)write the 17 PNGs + docs copies
    python tools/make_app_screenshots.py --self-test  # red canaries: --check can fail
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import io
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FIXTURES = TOOLS / "host" / "fixtures" / "apps"
BASELINE = FIXTURES / "baseline"
OUT = ROOT / "screenshots" / "apps"
DOCS_APPS = ROOT / "docs" / "assets" / "apps-proof"

sys.path.insert(0, str(TOOLS))
import make_webapp_proof as wp  # noqa: E402 - the ONE font/cell/palette model
import visual_proof_gate as vg  # noqa: E402 - the ONE visual quality model
from p4test import screenshot as shot  # noqa: E402 - the ONE pixel model

PANEL_W, PANEL_H = wp.PANEL_W, wp.PANEL_H
CELL_W, CELL_H = wp.CELL_W, wp.CELL_H
COLS, ROWS = wp.COLS, wp.ROWS
GRID_X, GRID_Y = wp.GRID_X, wp.GRID_Y
FOOTER_Y = wp.FOOTER_Y

# The 16 APPLIB-tiled dashboards plus the flagship SHOWCASE. SNAKE (arena),
# WEB/WEBAPP (HTML page) and companion LIB are non-headless in the gate but
# have no tiled APPLIB dashboard, so they are deliberately not in this set.
APPS: Tuple[str, ...] = (
    "ADVENT", "TCMD", "MOOD", "CHAT", "DIAG", "ELITE", "NOTES", "PALMTOP",
    "PICS", "TASKMGR", "COMPANION", "SYS", "NET", "FILES", "FUN", "SET",
    "SHOWCASE",
)

# Shipped DOS palette indices (make_webapp_proof.PALETTE_NAMES).
THEME_FG = {"amber": 11, "green": 10, "ice": 14, "mono": 7}
RICH_FG = 14       # bright cyan for [M-RICH-*] transcript lines
DONE_FG = 10       # bright green for the batch done marker
MUTED_FG = 8       # bright black for exit/ERRORLEVEL lines
PLAIN_FG = 7       # DOS light grey / default background fg
DEFAULT_CELL_FG = 16  # tui_frame default: remapped, never rendered as a colour

NOTICE = "HOST PROOF - NOT A DEVICE CAPTURE"
BAR_BG = (24, 20, 14)          # warm DOS chrome strip, not a new palette
SHELL_TOKENS = ("P4>", "p4>", "C:\\>", "shell>", "VER ", "dir /")
CELL_BEGIN = "=== TUI CELL GRID ==="
CELL_END = "=== TUI CELL GRID END ==="


# ---------------------------------------------------------------------------
# The ONE existing gates - imported, never copied.
# ---------------------------------------------------------------------------
def load_module(name: str, filename: str):
    path = TOOLS / filename
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the existing gate: %s" % path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses looks the module up here
    spec.loader.exec_module(module)
    return module


def load_apps_gate():
    return load_module("p4_host_apps_gate", "host_apps_gate.py")


def load_tui_gate():
    """The cell-report parser host_tui_gate.py already uses for scene proof."""
    return load_module("p4_host_tui_gate", "host_tui_gate.py")


def load_palette() -> List[Tuple[int, int, int]]:
    return wp.load_palette()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# The gated 80x25 cell grid, decoded from the real tui_frame_t report.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class AppFacts:
    name: str
    transcript: Tuple[str, ...]
    structure: object          # host_apps_gate.DosStructure
    rows: Tuple[str, ...]      # 25 real cell rows, each exactly 80 glyphs
    fgs: Tuple[Tuple[int, ...], ...]  # one DOS palette index per cell


def cell_section(raw: str, fixture: str) -> str:
    """Extract the real cell report printed after the batch ERRORLEVEL line."""
    lines = [line.rstrip() for line in raw.replace("\r\n", "\n").split("\n")]
    if lines.count(CELL_BEGIN) != 1 or lines.count(CELL_END) != 1:
        raise RuntimeError("%s did not print exactly one real cell grid" % fixture)
    begin = lines.index(CELL_BEGIN)
    end = lines.index(CELL_END)
    if end <= begin:
        raise RuntimeError("%s cell grid markers are out of order" % fixture)
    return "\n".join(lines[begin + 1:end]) + "\n"


def parse_cell_dump(raw: str, fixture: str) -> Tuple[List[str], List[List[int]]]:
    """Validate the tui dump with host_tui_gate, then decode it to 80x25 cells.

    Four ASCII hex digits per cell keep the ELF report cp1252-safe; the glyph
    itself is the real UTF-8 codepoint written into the one tui_frame_t.
    """
    htg = load_tui_gate()
    canonical, counts = htg.parse_emission(cell_section(raw, fixture))
    if counts["rows"] != ROWS or counts["runs"] != ROWS:
        raise RuntimeError("%s cell grid is %d rows / %d runs (need %d/%d)"
                           % (fixture, counts["rows"], counts["runs"], ROWS, ROWS))
    glyphs: Dict[int, str] = {}
    colours: Dict[int, List[int]] = {}

    for line in canonical:
        m = htg.ROW_RE.match(line)
        if m:
            index = int(m.group(1))
            tokens = m.group(2).split()
            if not 1 <= index <= ROWS or len(tokens) > COLS:
                raise RuntimeError("%s has an invalid cell row %d" % (fixture, index))
            row = []
            for token in tokens:
                if len(token) != 4 or any(c not in "0123456789ABCDEF" for c in token):
                    raise RuntimeError("%s cell row %d has a bad glyph token %r"
                                       % (fixture, index, token))
                row.append(chr(int(token, 16)))
            while len(row) < COLS:
                row.append(" ")
            glyphs[index] = "".join(row[:COLS])
            continue
        m = htg.RUN_RE.match(line)
        if m:
            index = int(m.group(1))
            if not 1 <= index <= ROWS or index in colours:
                raise RuntimeError("%s has an invalid colour run row %d" % (fixture, index))
            fgs = [PLAIN_FG] * COLS
            for item in m.group(2).split():
                span, colour = item.rsplit(":", 1)
                first, last = (int(v) for v in span.split("-", 1))
                value = int(colour)
                if not 1 <= first <= last <= COLS or not 0 <= value <= 16:
                    raise RuntimeError("%s has an invalid colour run %r"
                                       % (fixture, item))
                for col in range(first - 1, last):
                    fgs[col] = PLAIN_FG if value == DEFAULT_CELL_FG else value
            colours[index] = fgs

    if len(glyphs) != ROWS or len(colours) != ROWS:
        raise RuntimeError("%s cell grid is incomplete (%d rows, %d run rows)"
                           % (fixture, len(glyphs), len(colours)))
    rows = [glyphs[i] for i in range(1, ROWS + 1)]
    fgs = [colours[i] for i in range(1, ROWS + 1)]
    if any(len(row) != COLS for row in rows) or any(len(fg) != COLS for fg in fgs):
        raise RuntimeError("%s cell grid is not 80 columns wide" % fixture)
    # A vacuous all-space grid is not a dashboard, however well it parses.
    if sum(1 for row in rows for ch in row if ch != " ") < 200:
        raise RuntimeError("%s cell grid has no dashboard content" % fixture)
    return rows, fgs


# ---------------------------------------------------------------------------
# Paint the gated cell grid with the ONE shipped font and palette.
# ---------------------------------------------------------------------------
def chrome(panel: Image.Image, name: str, structure) -> None:
    """A DOS window frame above the gate-validated cell grid."""
    theme_colour = load_palette()[THEME_FG[structure.theme]]
    yellow = load_palette()[11]
    cyan = load_palette()[14]
    draw = ImageDraw.Draw(panel)

    window = "P4 MINI SHELL - APP HOST PROOF"
    top = "┌─[" + window + "]" + "─" * (COLS - len(window) - 5) + "┐"
    _bar_row(panel, 6, top, yellow)

    draw.rectangle((GRID_X - 9, GRID_Y - 3,
                    GRID_X + COLS * CELL_W + 8, FOOTER_Y + 2),
                   outline=BAR_BG)
    for row in range(ROWS):
        y = GRID_Y + row * CELL_H
        wp.paste_cell(panel, "│", theme_colour, GRID_X - 8, y)
        wp.paste_cell(panel, "│", theme_colour, GRID_X + COLS * CELL_W, y)

    _bar_row(panel, FOOTER_Y + 8, "└" + "─" * (COLS - 2) + "┘", BAR_BG)

    notice_font = ImageFont.truetype(str(wp.TTF), 11)
    caption_font = ImageFont.truetype(str(wp.TTF), 11)
    caption = "%s | %s" % (name, structure.screen)
    draw.text((16, FOOTER_Y + 43), caption, fill=cyan, font=caption_font)
    width = int(draw.textlength(NOTICE, font=notice_font))
    draw.text((PANEL_W - width - 16, FOOTER_Y + 43), NOTICE,
              fill=yellow, font=notice_font)


def _bar_row(panel: Image.Image, y: int, text: str,
             colour: Tuple[int, int, int]) -> None:
    text = text[:COLS].ljust(COLS)
    ImageDraw.Draw(panel).rectangle((GRID_X - 8, y - 2,
                                     GRID_X + COLS * CELL_W + 7,
                                     y + CELL_H + 1), fill=BAR_BG)
    for col, char in enumerate(text):
        wp.paste_cell(panel, char, colour, GRID_X + col * CELL_W, y)


def build(facts: AppFacts, palette: Sequence[Tuple[int, int, int]]) -> Image.Image:
    panel = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    chrome(panel, facts.name, facts.structure)
    for index in range(ROWS):
        line = facts.rows[index]
        for col, char in enumerate(line[:COLS]):
            wp.paste_cell(panel, char, palette[facts.fgs[index][col]],
                          GRID_X + col * CELL_W, GRID_Y + index * CELL_H)
    return panel


# ---------------------------------------------------------------------------
# Fresh, gate-validated facts from the real interpreter, never a fake backend.
# ---------------------------------------------------------------------------
def fresh(name: str):
    ag = load_apps_gate()
    hbr = ag.load_batch_runner()
    runner = hbr.discover_elf(None)
    raw = hbr.run_elf(runner, FIXTURES, name)
    note, result = hbr.evaluate(raw, name, BASELINE, update=False)
    structure = ag.parse_dos_structure(result.transcript, name)
    rows, fgs = parse_cell_dump(raw, name)
    facts = AppFacts(name, tuple(result.transcript), structure,
                     tuple(rows), tuple(tuple(r) for r in fgs))
    return facts, build(facts, load_palette()), note


def fresh_all():
    ag = load_apps_gate()
    seen: Dict[str, str] = {}
    items = []
    for name in APPS:
        facts, image, note = fresh(name)
        ag.register_structure(seen, name, facts.structure)
        items.append((name, image, facts, note))
    return items


# ---------------------------------------------------------------------------
# Verification. The visual model is the shared visual_proof_gate, not a copy.
# ---------------------------------------------------------------------------
def to_bmp(img: Image.Image) -> shot.Bmp:
    buf = io.BytesIO()
    img.convert("RGB").save(buf, format="BMP")
    return shot.parse_bmp(buf.getvalue())


def signature(img: Image.Image) -> bytes:
    return vg.signature(to_bmp(img))


def verify(img: Image.Image, expected: Image.Image, facts: AppFacts,
           label: str) -> Dict[str, bool]:
    out: Dict[str, bool] = {}
    out["size_" + label] = img.size == (PANEL_W, PANEL_H) and img.mode == "RGB"
    if not out["size_" + label]:
        return out
    out["deterministic_" + label] = img.tobytes() == expected.tobytes()

    defects = vg.analyse(to_bmp(img), framed=True)
    out["visual_quality_" + label] = not defects
    if defects:
        print("    visual: " + "; ".join(defects))

    joined = "\n".join(facts.rows)
    out["cell_grid_" + label] = (
        len(facts.rows) == ROWS and
        all(len(row) == COLS for row in facts.rows) and
        len(facts.fgs) == ROWS and
        all(len(row) == COLS and all(0 <= v <= 15 for v in row)
            for row in facts.fgs))
    out["real_cells_" + label] = (
        facts.name in joined and facts.structure.head in joined and
        any(ch in joined for ch in "┌┐└┘╔╗╚╝┏┓┗┛"))
    panels = sum(joined.count(ch) for ch in "┌╔┏")
    out["tiled_panels_" + label] = panels >= facts.structure.panels
    st = facts.structure
    transcript = "\n".join(facts.transcript)
    out["dos_identity_" + label] = (
        st.screen in transcript and st.head in transcript and
        "theme=" + st.theme in transcript and
        "panels=%d" % st.panels in transcript)
    out["box_frame_" + label] = (
        facts.rows[0].startswith(("┌", "╔", "┏")) and
        facts.rows[0].endswith(("┐", "╗", "┓")) and
        facts.rows[1].startswith(("│", "║")) and
        facts.rows[2].startswith(("└", "╚", "┗")) and
        "─" in facts.rows[0] or "═" in facts.rows[0])
    out["no_shell_chrome_" + label] = (
        not any(token in joined for token in SHELL_TOKENS))
    return out


def check_distinct(sigs: Dict[str, bytes]) -> List[str]:
    fails: List[str] = []
    names = sorted(sigs)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            dist = vg.sig_distance(sigs[a], sigs[b])
            if dist <= vg.DUP_L1:
                fails.append("%s and %s are the same screen (L1=%d)"
                             % (a, b, dist))
    return fails


def report(results: Dict[str, bool]) -> None:
    for key in sorted(results):
        print("  [%s] %s" % ("PASS" if results[key] else "FAIL", key))


def update() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    DOCS_APPS.mkdir(parents=True, exist_ok=True)
    for name, image, _, _ in fresh_all():
        path = OUT / (name + ".png")
        mirror = DOCS_APPS / (name + ".png")
        image.save(path)
        image.save(mirror)
        print("wrote %s + %s sha256=%s (host proof; not a device capture)"
              % (path.relative_to(ROOT), mirror.relative_to(ROOT), sha256(path)))
    return 0


def check() -> int:
    sigs: Dict[str, bytes] = {}
    ok = True
    items = fresh_all()
    for name, expected, facts, note in items:
        path = OUT / (name + ".png")
        mirror = DOCS_APPS / (name + ".png")
        if not path.is_file() or not mirror.is_file():
            print("check: MISSING %s or %s (run --update)"
                  % (path.relative_to(ROOT), mirror.relative_to(ROOT)))
            return 1
        with Image.open(path) as handle:
            img = handle.convert("RGB")
        results = verify(img, expected, facts, name)
        report(results)
        good = all(results.values())
        identical = path.read_bytes() == mirror.read_bytes()
        print("  [%s] docs_mirror_%s" % ("PASS" if identical else "FAIL", name))
        good = good and identical
        ok = ok and good
        sigs[name + ".png"] = signature(img)
        print("check %s: %s sha256=%s (%s; host proof, not a device capture)"
              % (name, "PASS" if good else "FAIL", sha256(path), note))
    duplicates = check_distinct(sigs)
    for item in duplicates:
        print("  FAIL " + item)
    if duplicates:
        ok = False
    print("make_app_screenshots CHECK: %d app proof(s), %d duplicate(s) -> %s"
          % (len(sigs), len(duplicates), "PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# Red canaries. A guard only seen green is not a guard. No ELF is needed here.
# ---------------------------------------------------------------------------
def _synthetic():
    """A deterministic, unpublished cell grid used only to exercise guards."""
    ag = load_apps_gate()
    htg = load_tui_gate()
    line = ('[M-DOS-CANARY] screen=CANARY-DEMO panels=3 head="CANARY" '
            'theme=ice bars=2 table=Y spark=1 status=Y')
    structure = ag.parse_dos_structure([line], "CANARY")
    transcript = (
        "[M-RICH-CANARY] host language extract",
        line,
        "[M-HOST-BATCH-DONE]",
        "ERRORLEVEL 0",
    )
    rows, fgs = [], []
    for r in range(1, ROWS + 1):
        row = [" "] * COLS
        fg = [PLAIN_FG] * COLS
        # Title band (rows 1-3), two tiled panels (5-11), a third panel
        # (13-19) and a status band (21-25): three real framed panels.
        if r == 1:
            row[0], row[-1] = "┌", "┐"
            for c in range(1, COLS - 1):
                row[c] = "─"
            for c, ch in enumerate("CANARY"):
                row[5 + c] = ch
            fg = [14] * COLS
        elif r in (3, 11, 19):
            row[0], row[-1] = "└", "┘"
            for c in range(1, COLS - 1):
                row[c] = "─"
            fg = [14] * COLS
        elif r == 5 or r == 13 or r == 21:
            row[0], row[-1] = "┌", "┐"
            for c in range(1, COLS - 1):
                row[c] = "─"
            fg = [14] * COLS
        elif r in (2, 6, 7, 8, 9, 10, 14, 15, 16, 17, 18, 22, 23, 24):
            row[0] = row[-1] = "│"
            if 6 <= r <= 10 or 14 <= r <= 18:
                row[38] = "│"
                row[60] = "│"
            fg = [14] * COLS
        elif r == ROWS:
            row[0], row[-1] = "└", "┘"
            for c in range(1, COLS - 1):
                row[c] = "─"
            fg = [14] * COLS
        else:
            for c in range(4, 40):
                row[c] = "#" if (c + r) % 3 else "."
            fg = [11] * COLS
        rows.append("".join(row))
        fgs.append(fg)

    # Exercise the gate's own parser on a real tui-style report too.
    sample = ["tui.scene.begin name=CANARY cols=80 rows=25",
              "001|250C " + "2500 " * 78 + "2510",
              "001@1-80:14",
              "tui.scene.dirty any=yes rect=1,1-80,25",
              "tui.scene.clean any=no", "tui.scene.end name=CANARY"]
    htg.parse_emission("\n".join(sample) + "\n")
    facts = AppFacts("CANARY", transcript, structure,
                     tuple(rows), tuple(tuple(r) for r in fgs))
    return facts, build(facts, load_palette())


def self_test() -> int:
    facts, good = _synthetic()
    checks: Dict[str, bool] = {}
    green = verify(good, good, facts, "CANARY")
    checks["green honest render passes"] = all(green.values())

    tampered = good.copy()
    tampered.putpixel((GRID_X + 4, GRID_Y + 4), (255, 0, 255))
    checks["red: tampered pixel rejected"] = not verify(
        tampered, good, facts, "CANARY")["deterministic_CANARY"]

    blank = Image.new("RGB", (PANEL_W, PANEL_H), wp.BG)
    checks["red: blank frame rejected"] = not verify(
        blank, blank, facts, "CANARY")["visual_quality_CANARY"]

    wrong_palette = good.copy()
    px = wrong_palette.load()
    for y in range(PANEL_H):
        for x in range(PANEL_W):
            if px[x, y] != wp.BG:
                px[x, y] = (0, 255, 0)
    checks["red: wrong palette rejected"] = not verify(
        wrong_palette, wrong_palette, facts,
        "CANARY")["visual_quality_CANARY"]

    no_border_rows = list(facts.rows)
    no_border_rows[0] = "X" * COLS
    no_border_rows[2] = "X" * COLS
    no_border = AppFacts("CANARY", facts.transcript, facts.structure,
                         tuple(no_border_rows), facts.fgs)
    checks["red: missing box border rejected"] = not verify(
        good, good, no_border, "CANARY")["box_frame_CANARY"]

    plain = ["X" * COLS for _ in range(ROWS)]
    panel_less = AppFacts("CANARY", facts.transcript, facts.structure,
                          tuple(plain), facts.fgs)
    checks["red: panel-less grid rejected"] = not verify(
        good, good, panel_less, "CANARY")["tiled_panels_CANARY"]

    short = list(facts.rows)
    short[24] = ""
    truncated = AppFacts("CANARY", facts.transcript, facts.structure,
                         tuple(short), facts.fgs)
    checks["red: truncated cell grid rejected"] = not verify(
        good, good, truncated, "CANARY")["cell_grid_CANARY"]

    shell_rows = list(facts.rows)
    shell_rows[4] = "P4> VER / fake shell transcript posed as an app".ljust(COLS)
    shell = AppFacts("CANARY", facts.transcript, facts.structure,
                     tuple(shell_rows), facts.fgs)
    checks["red: shell chrome rejected"] = not verify(
        good, good, shell, "CANARY")["no_shell_chrome_CANARY"]

    blue = good.copy()
    ImageDraw.Draw(blue).rectangle((400, 560, 600, 590),
                                   fill=(0x21, 0x96, 0xF3))
    checks["red: LVGL default-blue blob rejected"] = not verify(
        blue, blue, facts, "CANARY")["visual_quality_CANARY"]

    sig = signature(good)
    duplicates = check_distinct({"a.png": sig, "b.png": sig})
    checks["red: two identical apps rejected"] = bool(duplicates)

    # The cell report parser must reject a grid with no colour runs.
    htg = load_tui_gate()
    try:
        htg.parse_emission("\n".join([
            "tui.scene.begin name=C cols=80 rows=1", "001|ABC",
            "tui.scene.dirty any=yes rect=1,1-3,1",
            "tui.scene.clean any=no", "tui.scene.end name=C",
        ]) + "\n")
        checks["red: colour runs required"] = False
    except htg.GateError:
        checks["red: colour runs required"] = True

    for key, ok in checks.items():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", key))
    ok = all(checks.values())
    print("make_app_screenshots self-test: %s (%d canaries)"
          % ("PASS" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="committed PNGs == a fresh gated render (default)")
    mode.add_argument("--update", action="store_true",
                      help="(re)write the app proofs and their docs copies")
    parser.add_argument("--self-test", action="store_true",
                        help="red canaries for the proof guards (no ELF needed)")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        if args.update:
            return update()
        return check()
    except Exception as exc:  # noqa: BLE001 - refuse to emit an imitation
        print("make_app_screenshots: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
