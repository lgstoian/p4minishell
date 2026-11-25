#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_browser_gate.py - REAL-EXECUTION host gate for browser/webapp features.

This is not a source grep and not a second renderer. It discovers the shipped
``host_webapp_render.elf`` (preferring the canonical ``test/host/build-wsl``
lane), WSL-routes the POSIX ELF on Windows, executes it on five real HTML
fixtures, and asserts what the ONE firmware DOM renderer actually emitted:

  * ``wiki_esp32.html``   - the ESP32 H1, the Espressif Systems anchor drawn in
    fg=11 bright cyan, 240 MHz, the infobox rows, and BOTH image outcomes
    (a resized BMP and a missing BMP with visible alt text);
  * the four DOS showcase fixtures - their headings, framed DOS presentation,
    status lines, coloured anchor runs and honest image/canvas outcome lines.

Every fixture is also compared against an exact baseline captured from that
same real renderer. The baselines are embedded in this file so the gate owns
its contract without touching another gate's fixture/baseline directory.

A ``--self-test`` drives the SAME assertion functions with red canaries built
from the real captured emission: wrong page, missing infobox, uncoloured link,
silently dropped image, tofu glyph, blank render, and broken keyboard/link
wiring. A canary that fails to go red fails the self-test.

Static wiring checks are labelled as STATIC, never as runtime device proof.
There is no board: live HTTPS, a full multi-thousand-row page, real LVGL touch,
mouse and keyboard input, and a tappable link on glass remain DEVICE-ONLY.

Usage:
    python tools/host_browser_gate.py             # --check (default)
    python tools/host_browser_gate.py --check
    python tools/host_browser_gate.py --self-test
    python tools/host_browser_gate.py --renderer PATH
"""
from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"


class GateError(RuntimeError):
    """Any condition that must make this gate red."""


def _load_shipped_gate():
    """Import the shipped host webapp gate as a library; do not copy its parser."""
    spec = importlib.util.spec_from_file_location(
        "p4_browser_host_webapp_gate", TOOLS / "host_webapp_gate.py")
    if spec is None or spec.loader is None:
        raise GateError("cannot import tools/host_webapp_gate.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


hwg = _load_shipped_gate()

FIXTURE_DIR = hwg.FIXTURES
FIXTURES: Tuple[str, ...] = (
    "wiki_esp32.html", "portal.html", "dashboard.html",
    "article.html", "form.html",
)
SHOWCASE = frozenset(FIXTURES[1:])

# Exact canonical emission of the ONE shipped host renderer, captured from a
# real run (not authored here). Regenerate with --print-baselines if the
# renderer legitimately changes, then review the diff before committing.
BASELINES = {
    'wiki_esp32.html': [
        'webapp.image kind=img src=esp32-module.bmp state=resized native=64x8 fit=32x2 alt=ESP-WROOM-32 module',
        'webapp.image kind=img src=esp32-die-missing.bmp state=missing native=64x8 fit=0x0 alt=ESP32 die shot unavailable',
        'webapp.render.begin fixture=wiki_esp32.html cols=80 rows=25',
        'tui.dump cols=80 from=1 to=25',
        '001|',
        '001@1-80:16',
        '002|ESP32byEspressif Systems-',
        '002@1-7:7 8-24:11 25-26:7 27-80:16',
        '003|240 MHz dual-core, Wi-Fi and Bluetooth on one die.',
        '003@1-50:7 51-80:16',
        '004|',
        '004@1-80:16',
        '005|Guide:official ESP-IDF page- modules,',
        '005@1-6:7 7-27:11 28-38:7 39-80:16',
        '006|toolchains and bindings.',
        '006@1-24:7 25-80:16',
        '007|+--------------------------------+',
        '007@1-34:7 35-80:16',
        '008||..=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-|',
        '008@1-1:7 2-3:0 4-33:8 34-34:7 35-80:16',
        '009||.........-  .........  .........|',
        '009@1-1:7 2-10:0 11-11:8 12-13:0 14-22:8 23-24:0 25-25:8 26-33:0 34-34:7 35-80:16',
        '010|+--------------------------------+',
        '010@1-34:7 35-80:16',
        '011|[ESP32 die shot unavailable]',
        '011@1-28:7 29-80:16',
        '012|ESP32',
        '012@1-5:14 6-80:16',
        '013|=====',
        '013@1-5:14 6-80:16',
        '014|',
        '014@1-80:16',
        '015|ESP32is a family of low-cost, energy-efficient microcontrollers that',
        '015@1-69:7 70-80:16',
        '016|support both Wi-Fi and Bluetooth. The chips are designed byEspressif Systemsand',
        '016@1-59:7 60-76:11 77-80:7',
        '017|combine a',
        '017@1-10:7 11-80:16',
        '018|Tensilica Xtensa or RISC-V processor with wireless radios, SRAM and peripheral',
        '018@1-79:7 80-80:16',
        '019|controllers on one die.',
        '019@1-23:7 24-80:16',
        '020|',
        '020@1-80:16',
        '021|ESP32         Microcontroller',
        '021@1-12:14 13-14:7 15-48:14 49-80:16',
        '022|------------------------------------------------',
        '022@1-48:7 49-80:16',
        '023|Manufacturer  Espressif Systems',
        '023@1-12:14 13-48:7 49-80:16',
        '024|------------------------------------------------',
        '024@1-48:7 49-80:16',
        '025|Type          Microcontroller',
        '025@1-12:14 13-48:7 49-80:16',
        'tui.dump.end',
        'webapp.meta semantic=1 controls=0 grids=0 canvases=0',
        'webapp.events handlers=0',
        'webapp.cascade h1_color=h1 spec=0.0.1 imp=0 order=2 fg=14',
        'webapp.entity &amp;=0026/5 &reg=00AE/4 &#x41;=0041/6 &#1114112;=FFFD/10 &#x80;=20AC/6 &hellip;=2026/8',
        'webapp.render.end fixture=wiki_esp32.html rows=25 runs=25',
    ],
    'portal.html': [
        'webapp.image kind=background src=pixel.bmp state=ok native=32x4 fit=32x4 alt=-',
        'webapp.image kind=img src=pixel.bmp state=resized native=32x4 fit=32x2 alt=P4 badge',
        'webapp.render.begin fixture=portal.html cols=80 rows=25',
        'tui.dump cols=80 from=1 to=25',
        '001|',
        '001@1-80:16',
        '002|┌─[P4.NET/PORTAL.80COL]────────────────────────────────────────────────────────┐',
        '002@1-80:14',
        '003|├─Address:http://p4.local/portal.html──────────────────────────────────────────┤',
        '003@1-80:14',
        '004|│ WELCOME TO P4 NET                              (C) 2026 P4 NET              │',
        '004@1-79:14 80-80:16',
        '005|│ ┌─  FEATURED  ────────────────────────────────────────────────────────────┐ │',
        '005@1-79:14 80-80:16',
        '006|│ │ RETRO WEB / REAL HTML / ONE DOS RENDERER                                │ │',
        '006@1-79:14 80-80:16',
        '007|│ │ Modern markup with a 1990s browser face.                                │ │',
        '007@1-79:14 80-80:16',
        '008|│ │ Entities: copyright (c) registered (r) trademark (tm)           │ │',
        '008@1-64:14 65-80:16',
        '009|│ └───────────────────────────────────────────────────────────────────────┘ │',
        '009@1-77:14 78-80:16',
        '010|+--------------------------------+',
        '010@1-34:7 35-80:16',
        '011||    ....========XXXX    ....====|',
        '011@1-1:7 2-9:0 10-13:6 14-17:3 18-21:7 22-29:0 30-33:6 34-34:7 35-80:16',
        '012||========XXXX    ....========XXXX|',
        '012@1-1:7 2-5:6 6-9:3 10-13:7 14-21:0 22-25:6 26-29:3 30-34:7 35-80:16',
        '013|+--------------------------------+',
        '013@1-34:7 35-80:16',
        '014|│ MAIN MENU                                              [ 01 / 03 ]          │',
        '014@1-79:14 80-80:16',
        '015|│   > 01  DASHBOARD    panels, tables and a canvas chart                      │',
        '015@1-79:14 80-80:16',
        '016|│   > 02  ARTICLE      columns, typography, reading view                      │',
        '016@1-79:14 80-80:16',
        '017|│   > 03  FORM         inputs, a select, submit handler                       │',
        '017@1-79:14 80-80:16',
        '018|│   1 Dashboard   2 Article   3 Form                                           │',
        '018@1-80:14',
        '019|│   Select a page to continue.                                                 │',
        '019@1-80:14',
        '020|│ STATUS Online | 80 columns | no network                                     │',
        '020@1-79:14 80-80:16',
        '021|├─READY | 3 links | press F1 for help──────────────────────────────────────────┤',
        '021@1-80:14',
        '022|└──────────────────────────────────────────────────────────────────────────────┘',
        '022@1-80:14',
        '023|',
        '023@1-80:16',
        '024|',
        '024@1-80:16',
        '025|',
        '025@1-80:16',
        'tui.dump.end',
        'webapp.meta semantic=4 controls=0 grids=0 canvases=0',
        'webapp.events handlers=0',
        'webapp.entity &amp;=0026/5 &reg=00AE/4 &#x41;=0041/6 &#1114112;=FFFD/10 &#x80;=20AC/6 &hellip;=2026/8',
        'webapp.render.end fixture=portal.html rows=25 runs=25',
    ],
    'dashboard.html': [
        'webapp.image kind=canvas src=chart state=resized native=64x8 fit=60x3 alt=-',
        'webapp.render.begin fixture=dashboard.html cols=80 rows=25',
        'tui.dump cols=80 from=1 to=25',
        '001|',
        '001@1-80:16',
        '002|┌─[P4.NET/DASHBOARD.80COL]─────────────────────────────────────────────────────┐',
        '002@1-80:14',
        '003|├─Address:http://p4.local/dashboard.html───────────────────────────────────────┤',
        '003@1-80:14',
        '004|│ LIVE DASHBOARD                              24-hour view / 5 nodes          │',
        '004@1-79:14 80-80:16',
        '005|',
        '005@1-80:16',
        '006|┌UPTIME 99.7%───────────────┐           ┌LOAD 0.42──────────────────┐',
        '006@1-29:11 30-40:16 41-69:11 70-80:16',
        '007|│ ┌─  TRAFFIC  ─────────────────────────────────────────────────────────────┐ │',
        '007@1-79:14 80-80:16',
        '008|+------------------------------------------------------------+',
        '008@1-62:7 63-80:16',
        '009||==============================XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX|',
        '009@1-1:7 2-31:12 32-61:11 62-62:7 63-80:16',
        '010||==============================XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX|',
        '010@1-1:7 2-31:12 32-61:11 62-62:7 63-80:16',
        '011||==============================XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX|',
        '011@1-1:7 2-31:12 32-61:11 62-62:7 63-80:16',
        '012|+------------------------------------------------------------+',
        '012@1-62:7 63-80:16',
        '013|',
        '013@1-80:16',
        '014|│ └───────────────────────────────────────────────────────────────────────┘ │',
        '014@1-77:14 78-80:16',
        '015|',
        '015@1-80:16',
        '016|NODE │ STATE │ MS',
        '016@1-17:7 18-80:16',
        '017|---------------------',
        '017@1-21:7 22-80:16',
        '018|alpha │ up │ 12',
        '018@1-17:7 18-80:16',
        '019|beta │ up │ 18',
        '019@1-17:7 18-80:16',
        '020|gamma │ warn │ 97',
        '020@1-17:7 18-80:16',
        '021|',
        '021@1-80:16',
        '022|│ TRAFFIC / LAST 24 HOURS              74% PEAK / 61% LOW                      │',
        '022@1-80:14',
        '023|├─READY | panels + table + chart | F1 help─────────────────────────────────────┤',
        '023@1-80:14',
        '024|└──────────────────────────────────────────────────────────────────────────────┘',
        '024@1-80:14',
        '025|',
        '025@1-80:16',
        'tui.dump.end',
        'webapp.meta semantic=4 controls=0 grids=1 canvases=1',
        'webapp.events handlers=0',
        'webapp.entity &amp;=0026/5 &reg=00AE/4 &#x41;=0041/6 &#1114112;=FFFD/10 &#x80;=20AC/6 &hellip;=2026/8',
        'webapp.canvas id=chart w=64 h=8 pixel0=ff5555ff pixel1=55ffffff grad0=ff0000ff grad1=0000ffff round_corner=00 round_centre=ff shadow=ff0000ff',
        'webapp.render.end fixture=dashboard.html rows=25 runs=25',
    ],
    'article.html': [
        'webapp.render.begin fixture=article.html cols=80 rows=25',
        'tui.dump cols=80 from=1 to=25',
        '001|',
        '001@1-80:16',
        '002|┌─[P4.NET/ARTICLE.80COL]───────────────────────────────────────────────────────┐',
        '002@1-80:14',
        '003|├─Address:http://p4.local/article.html─────────────────────────────────────────┤',
        '003@1-80:14',
        '004|',
        '004@1-80:16',
        '005|│ THE DOS BROWSER REVISITED              issue 07 / reading view              │',
        '005@1-79:14 80-80:16',
        '006|│ ┌─  LEAD  ────────────────────────────────────────────────────────────────┐ │',
        '006@1-79:14 80-80:16',
        '007|│ │ A modern page in an 80-column cell grid: real HTML,                     │ │',
        '007@1-79:14 80-80:16',
        '008|│ │ one cascade, one renderer, CP437 panels, status line.                   │ │',
        '008@1-79:14 80-80:16',
        '009|│ └───────────────────────────────────────────────────────────────────────┘ │',
        '009@1-77:14 78-80:16',
        '010|',
        '010@1-80:16',
        '011|│ Typography: -- em dash  ... ellipsis  EUR euro                 │',
        '011@1-61:14 62-80:16',
        '012|│ Temp 21degC   (c) 2026   (r) P4   (tm) mark                       │',
        '012@1-60:14 61-80:16',
        '013|',
        '013@1-80:16',
        '014|Columns keep the reading rhythm         short lines, wide gutters, clear type',
        '014@1-31:15 32-40:16 41-77:15 78-80:16',
        '015|',
        '015@1-80:16',
        '016|│ > Quoted: the grid is the interface, not a dump.                            │',
        '016@1-79:14 80-80:16',
        '017|│ > Read on: the article continues in two columns.                            │',
        '017@1-79:14 80-80:16',
        '018|',
        '018@1-80:16',
        '019|│ NEXT:Form/Portal│',
        '019@1-7:14 8-11:11 12-12:14 13-18:11 19-19:14 20-80:16',
        '020|',
        '020@1-80:16',
        '021|├─READY | 2-column reading view | F1 help──────────────────────────────────────┤',
        '021@1-80:14',
        '022|└──────────────────────────────────────────────────────────────────────────────┘',
        '022@1-80:14',
        '023|',
        '023@1-80:16',
        '024|',
        '024@1-80:16',
        '025|',
        '025@1-80:16',
        'tui.dump.end',
        'webapp.meta semantic=4 controls=0 grids=1 canvases=0',
        'webapp.events handlers=0',
        'webapp.entity &amp;=0026/5 &reg=00AE/4 &#x41;=0041/6 &#1114112;=FFFD/10 &#x80;=20AC/6 &hellip;=2026/8',
        'webapp.render.end fixture=article.html rows=25 runs=25',
    ],
    'form.html': [
        'webapp.render.begin fixture=form.html cols=80 rows=25',
        'tui.dump cols=80 from=1 to=25',
        '001|',
        '001@1-80:16',
        '002|┌─[P4.NET/FORM.80COL]──────────────────────────────────────────────────────────┐',
        '002@1-80:14',
        '003|├─Address:http://p4.local/form.html────────────────────────────────────────────┤',
        '003@1-80:14',
        '004|',
        '004@1-80:16',
        '005|│ ACCOUNT SETTINGS                                   form / 3 controls        │',
        '005@1-79:14 80-80:16',
        '006|│ ┌─  PROFILE  ─────────────────────────────────────────────────────────────┐ │',
        '006@1-79:14 80-80:16',
        '007|│ │ Name:User:[Ada]│ │',
        '007@1-22:14 23-80:16',
        '008|│ │ Plan[Pro]│ │',
        '008@1-16:14 17-80:16',
        '009|│ │ Notify[x]email                    │ │',
        '009@1-41:14 42-80:16',
        '010|│ │ Mode[x]fast                      │ │',
        '010@1-40:14 41-80:16',
        '011|│ └───────────────────────────────────────────────────────────────────────┘ │',
        '011@1-77:14 78-80:16',
        '012|',
        '012@1-80:16',
        '013|│[ Save ]Cancel │',
        '013@1-9:14 10-17:11 18-80:16',
        '014|│ RESULT: NOT SAVED YET                                                       │',
        '014@1-79:14 80-80:16',
        '015|',
        '015@1-80:16',
        '016|│ CHANGES APPEAR HERE AFTER YOU SAVE                                          │',
        '016@1-79:14 80-80:16',
        '017|',
        '017@1-80:16',
        '018|├─READY | submit saves settings | F1 help──────────────────────────────────────┤',
        '018@1-80:14',
        '019|└──────────────────────────────────────────────────────────────────────────────┘',
        '019@1-80:14',
        '020|',
        '020@1-80:16',
        '021|',
        '021@1-80:16',
        '022|',
        '022@1-80:16',
        '023|',
        '023@1-80:16',
        '024|',
        '024@1-80:16',
        '025|',
        '025@1-80:16',
        'tui.dump.end',
        'webapp.meta semantic=4 controls=7 grids=0 canvases=0',
        'webapp.events handlers=1',
        'webapp.entity &amp;=0026/5 &reg=00AE/4 &#x41;=0041/6 &#1114112;=FFFD/10 &#x80;=20AC/6 &hellip;=2026/8',
        'webapp.render.end fixture=form.html rows=25 runs=25',
    ],
}

RUN_ITEM_RE = re.compile(r"^(\d+)-(\d+):(\d+)$")
TOFU = ("\u25a1", "\u25af", "\u23a5", "\ufffd")  # □ ▯ ⎕ U+FFFD
DEBUG_TOKENS = ("\\u00", "webapp.", "dom_", "css_", "debug", "trace")

MIN_ROWS = 20
MIN_NONBLANK_ROWS = 10
MIN_RUNS = 20
MIN_INK = 300
MAX_COLS = 80
FG_LINK = 11          # DOS bright cyan: the anchor colour in the real dump
FG_BG = 16            # transparent/background: not an inked foreground

# Static wiring facts (STATIC only: no runtime/device claim is made here).
WIRING_PATHS = {
    "commands": ROOT / "components" / "command" / "browser_commands.c",
    "dom": ROOT / "components" / "markdown" / "dom_render.c",
    "tui": ROOT / "components" / "tui" / "tui.c",
    "modal": ROOT / "components" / "modal" / "modal_surf.h",
    "shell": ROOT / "components" / "command" / "command.c",
}


# ---------------------------------------------------------------------------
# Emission helpers: every input here is what the real renderer printed.
# ---------------------------------------------------------------------------
def parse(raw: str, fixture: str):
    return hwg.parse_emission(raw, fixture, FIXTURE_DIR)


def baseline_raw(fixture: str) -> str:
    lines = BASELINES.get(fixture)
    if not lines:
        raise GateError("no embedded baseline for %s" % fixture)
    return "\n".join(lines) + "\n"


def row_has(result, needle: str) -> bool:
    return any(needle in text for text in result.rows.values())


def coloured_spans(result, needle: str,
                   fg: Optional[int]) -> List[Tuple[int, int, int]]:
    """(row, first_col, last_col) where @p needle sits inside a matching run.

    fg=None means "any inked foreground" (not the transparent/background 16):
    the exact h1 grey/black shade is a CSS decision, but a heading must still
    be painted with some foreground rather than silently falling back to bg.
    """
    spans: List[Tuple[int, int, int]] = []
    for index, text in result.rows.items():
        pos = text.find(needle)
        while pos >= 0:
            start, end = pos + 1, pos + len(needle)
            for item in result.runs.get(index, ()):
                match = RUN_ITEM_RE.match(item)
                if match is None:
                    continue
                lo, hi, colour = (int(match.group(1)), int(match.group(2)),
                                  int(match.group(3)))
                matches = colour != FG_BG if fg is None else colour == fg
                if matches and lo <= start and hi >= end:
                    spans.append((index, start, end))
                    break
            pos = text.find(needle, pos + 1)
    return spans


def image_states(result) -> List[str]:
    states = []
    for line in result.images:
        match = hwg.IMAGE_RE.match(line)
        if match is not None:
            states.append(match.group(3))
    return states


def shape_fails(result) -> List[str]:
    """Reject a blank, tiny, over-wide or tofu-filled render."""
    fails: List[str] = []
    if len(result.rows) < MIN_ROWS:
        fails.append("only %d row(s), expected >= %d (blank/tiny render)"
                     % (len(result.rows), MIN_ROWS))
    nonblank = [idx for idx, text in result.rows.items() if text.strip()]
    if len(nonblank) < MIN_NONBLANK_ROWS:
        fails.append("only %d non-blank row(s), expected >= %d"
                     % (len(nonblank), MIN_NONBLANK_ROWS))
    ink = sum(len(text.strip()) for text in result.rows.values())
    if ink < MIN_INK:
        fails.append("only %d visible cell(s), expected >= %d" % (ink, MIN_INK))
    if len(result.runs) < MIN_RUNS:
        fails.append("only %d colour run(s), expected >= %d"
                     % (len(result.runs), MIN_RUNS))
    for index, text in sorted(result.rows.items()):
        if len(text) > MAX_COLS:
            fails.append("row %03d is %d columns wide (> %d)"
                         % (index, len(text), MAX_COLS))
        for glyph in TOFU:
            if glyph in text:
                fails.append("row %03d has tofu/empty-box glyph %r" % (index, glyph))
        low = text.lower()
        for token in DEBUG_TOKENS:
            if token in low:
                fails.append("row %03d has raw debug token %r" % (index, token))
    return fails


def wiki_fails(result) -> List[str]:
    """Assert the wiki page's ACTUAL emitted content, not its source HTML."""
    fails: List[str] = []
    if not any(text.startswith("ESP32") for text in result.rows.values()):
        fails.append("infobox/wiki H1 'ESP32' was not emitted as a row prefix")
    if not coloured_spans(result, "ESP32", None):
        fails.append("'ESP32' H1 has no inked foreground run (heading colour lost)")
    if not coloured_spans(result, "Espressif Systems", FG_LINK):
        fails.append("'Espressif Systems' anchor has no fg=%d bright-cyan run" % FG_LINK)
    if not row_has(result, "240 MHz"):
        fails.append("'240 MHz' specification was silently dropped")
    for needle in ("Manufacturer", "Microcontroller", "Espressif Systems"):
        if not row_has(result, needle):
            fails.append("infobox row %r is missing" % needle)
    states = image_states(result)
    for state in ("resized", "missing"):
        if state not in states:
            fails.append("image outcome %r was silently dropped" % state)
    if len(result.images) < 2:
        fails.append("expected >= 2 image outcome lines, got %d" % len(result.images))
    for line in result.images:
        match = hwg.IMAGE_RE.match(line)
        if match is not None and match.group(3) == "missing":
            if match.group(8).strip() in ("", "-"):
                fails.append("missing image has no visible alt text: %s" % line)
    return fails


EXPECTED_HEADINGS = {
    "portal.html": ("WELCOME TO P4 NET", "STATUS", "READY", "3 links"),
    "dashboard.html": ("LIVE DASHBOARD", "NODE", "STATE", "TRAFFIC"),
    "article.html": ("THE DOS BROWSER REVISITED", "NEXT:"),
    "form.html": ("ACCOUNT SETTINGS", "RESULT: NOT SAVED YET"),
}
LINKED_TEXT = {
    "article.html": "Form",
    "form.html": "Cancel",
}


def showcase_fails(result, fixture: str) -> List[str]:
    fails: List[str] = []
    for needle in EXPECTED_HEADINGS.get(fixture, ()):
        if not row_has(result, needle):
            fails.append("%r was not emitted on the rendered page" % needle)
    fails.extend(hwg.check_dos_presentation(result))
    fails.extend(hwg.check_image_reports(result))
    needle = LINKED_TEXT.get(fixture)
    if needle is not None and not coloured_spans(result, needle, FG_LINK):
        fails.append("anchor %r has no fg=%d bright-cyan run" % (needle, FG_LINK))
    states = set(image_states(result))
    if fixture == "portal.html" and not {"ok", "resized"} <= states:
        fails.append("portal must report ok + resized image outcomes, got %s"
                     % sorted(states))
    if fixture == "dashboard.html" and "canvas" not in {
            hwg.IMAGE_RE.match(line).group(1) for line in result.images
            if hwg.IMAGE_RE.match(line)}:
        fails.append("dashboard canvas outcome line was silently dropped")
    return fails


# ---------------------------------------------------------------------------
# STATIC wiring checks. They prove the chain exists in the shipped source;
# they do NOT prove a tap on glass worked. No LVGL is fabricated here.
# ---------------------------------------------------------------------------
def _line_hits(text: str, pattern: str) -> List[int]:
    return [i + 1 for i, line in enumerate(text.splitlines())
            if re.search(pattern, line)]


def static_wiring_checks(sources: Optional[Dict[str, str]] = None
                         ) -> List[Tuple[str, bool, str]]:
    if sources is None:
        sources = {key: path.read_text(encoding="utf-8")
                   for key, path in WIRING_PATHS.items()}
    checks: List[Tuple[str, bool, str]] = []

    def require(label: str, key: str, pattern: str) -> None:
        hits = _line_hits(sources[key], pattern)
        evidence = ", ".join("%s:%d" % (WIRING_PATHS[key].name, n)
                             for n in hits[:2]) or "NOT FOUND"
        checks.append((label, bool(hits), evidence))

    require("STATIC dom_render emits put_link regions", "dom",
            r"c->ops->put_link\(")
    require("STATIC browser installs put_link sink", "commands",
            r"ops\.put_link\s*=\s*browser_put_link")
    require("STATIC browser_put_link reaches tui_link_register", "commands",
            r"tui_link_register\(")
    require("STATIC browser_init installs link callback", "commands",
            r"tui_set_link_callback\(browser_link_run\)")
    require("STATIC tui hit-tests with scroll offset", "tui",
            r"tui_link_hit_test\(col,\s*s_scroll_offset\s*\+\s*row")
    require("STATIC link command lookup exists", "commands",
            r"tui_link_command_at\(")
    require("STATIC shell calls browser_init", "shell", r"browser_init\(\);")

    for name, constant in (("PageUp", "MODAL_KEY_PAGE_UP"),
                           ("PageDown", "MODAL_KEY_PAGE_DOWN")):
        lines = sources["commands"].splitlines()
        idx = next((i for i, line in enumerate(lines)
                    if "key_code == " + constant in line), None)
        window = "\n".join(lines[idx:idx + 5]) if idx is not None else ""
        sign = "-" if name == "PageUp" else ""
        ok = ("tui_viewport_scroll_by(%stui_viewport_get_page_rows())" % sign
              in window and "browser_page_present()" in window)
        evidence = ("browser_commands.c:%d" % (idx + 1)) if idx is not None \
            else "NOT FOUND"
        checks.append(("STATIC %s uses %s + page-row scroll + present"
                       % (name, constant), ok, evidence))

    for constant in ("MODAL_KEY_PAGE_UP", "MODAL_KEY_PAGE_DOWN"):
        require("STATIC %s constant defined" % constant, "modal",
                r"#define\s+" + constant + r"\b")
    return checks


# ---------------------------------------------------------------------------
# Gate driver.
# ---------------------------------------------------------------------------
def evaluate(raw: str, fixture: str) -> int:
    """One fixture: exact baseline + real emitted content contract."""
    result = parse(raw, fixture)
    hwg.compare_lines(result.lines, BASELINES[fixture], fixture)
    fails = shape_fails(result)
    fails += wiki_fails(result) if fixture == "wiki_esp32.html" \
        else showcase_fails(result, fixture)
    if fails:
        raise GateError("%s: %s" % (fixture, "; ".join(fails)))
    return len(result.rows) + len(result.runs) + len(result.images) + 1


def renderer_output(renderer: Path, fixture: str) -> str:
    """Run the real ELF; retry only a transient Windows/WSL ELF file lock."""
    for attempt in range(3):
        try:
            return hwg.run_renderer(renderer, FIXTURE_DIR / fixture, FIXTURE_DIR)
        except hwg.GateError as exc:
            if "WinError 32" in str(exc) and attempt < 2:
                time.sleep(0.25)
                continue
            raise
    raise GateError("cannot run renderer for %s" % fixture)


def run_gate(renderer: Path) -> int:
    if not renderer.is_file():
        raise GateError("renderer %s is missing (build the host lane first)"
                        % renderer)
    assertions = 0
    for fixture in FIXTURES:
        raw = renderer_output(renderer, fixture)
        assertions += evaluate(raw, fixture)
        result = parse(raw, fixture)
        print("  %-16s %2d rows, %2d runs, %d images - REAL RENDER OK"
              % (fixture, len(result.rows), len(result.runs), len(result.images)))
    wiring = static_wiring_checks()
    for label, ok, evidence in wiring:
        print("  [%s] %s (%s)" % ("PASS" if ok else "FAIL", label, evidence))
        if not ok:
            raise GateError("broken wiring: %s (%s)" % (label, evidence))
    assertions += len(wiring)
    print("browser gate: %d fixture(s), %d assertion(s), %d static wiring "
          "check(s) -> OK" % (len(FIXTURES), assertions, len(wiring)))
    print("NOTE: static wiring is NOT runtime device proof; live HTTPS, full "
          "page, real LVGL input and glass taps remain DEVICE-ONLY.")
    return 0


def _expect_red(label: str, fn, checks: List[Tuple[str, bool]],
                needle: Optional[str] = None) -> None:
    try:
        fails = fn()
    except hwg.GateError as exc:
        ok = needle is None or needle in str(exc)
        checks.append((label, ok))
        return
    if isinstance(fails, list):
        ok = any(needle in str(item) for item in fails) if needle else bool(fails)
        checks.append((label, ok))
    else:
        checks.append((label, False))


def self_test() -> int:
    """Red canaries: every tamper must be rejected by the same assertions."""
    wiki = BASELINES["wiki_esp32.html"]
    checks: List[Tuple[str, bool]] = []

    clean = parse(baseline_raw("wiki_esp32.html"), "wiki_esp32.html")
    checks.append(("clean real emission accepted",
                   not (shape_fails(clean) + wiki_fails(clean))))

    wrong = [line.replace("wiki_esp32.html", "portal.html") for line in wiki]
    _expect_red("wrong page rejected",
                lambda: parse("\n".join(wrong) + "\n", "wiki_esp32.html")
                and [], checks, "fixture mismatch")

    blanked = [re.sub(r"^(02[1-5])\|.*$", r"\1|", line) for line in wiki]
    _expect_red("missing infobox rejected",
                lambda: shape_fails(parse("\n".join(blanked) + "\n",
                                          "wiki_esp32.html"))
                + wiki_fails(parse("\n".join(blanked) + "\n",
                                   "wiki_esp32.html")),
                checks, "infobox")

    def flatten_runs(line: str, row: int) -> str:
        return re.sub(r"^%03d@.*$" % row, "%03d@1-80:0" % row, line)

    uncoloured = [flatten_runs(flatten_runs(line, 2), 16) for line in wiki]
    _expect_red("link not coloured rejected",
                lambda: wiki_fails(parse("\n".join(uncoloured) + "\n",
                                         "wiki_esp32.html")),
                checks, "bright-cyan")

    dropped = [line for line in wiki if not line.startswith(
        "webapp.image kind=img src=esp32-die-missing.bmp")]
    _expect_red("missing image silently dropped rejected",
                lambda: wiki_fails(parse("\n".join(dropped) + "\n",
                                         "wiki_esp32.html")),
                checks, "silently dropped")

    tofu = [line.replace("240 MHz", "240 \u25a1 MHz") for line in wiki]
    _expect_red("tofu glyph rejected",
                lambda: shape_fails(parse("\n".join(tofu) + "\n",
                                          "wiki_esp32.html")),
                checks, "tofu")

    tiny = "\n".join([
        hwg.BEGIN + " fixture=wiki_esp32.html cols=80 rows=2",
        hwg.DUMP_HEAD + " 80 from=1 to=2",
        "001|ESP32", "001@1-5:0", "002|ok", "002@1-2:0", hwg.DUMP_END,
        "webapp.meta semantic=0 controls=0 grids=0 canvases=0",
        hwg.END + " fixture=wiki_esp32.html rows=2 runs=2",
    ]) + "\n"
    _expect_red("blank/tiny render rejected",
                lambda: shape_fails(parse(tiny, "wiki_esp32.html")),
                checks, "blank/tiny")

    sources = {key: path.read_text(encoding="utf-8")
               for key, path in WIRING_PATHS.items()}
    broken = dict(sources)
    broken["commands"] = sources["commands"].replace(
        "tui_viewport_scroll_by(tui_viewport_get_page_rows());", "")
    _expect_red("broken PageDown wiring rejected",
                lambda: [item for item in static_wiring_checks(broken)
                         if not item[1]],
                checks, "PageDown")

    broken2 = dict(sources)
    broken2["commands"] = sources["commands"].replace(
        "tui_link_register(x, y, w, h, url);", "")
    _expect_red("broken link-register wiring rejected",
                lambda: [item for item in static_wiring_checks(broken2)
                         if not item[1]],
                checks, "tui_link_register")

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("browser gate self-test: %s (%d canaries)" % ("PASS" if ok else "FAIL",
                                                        len(checks)))
    return 0 if ok else 1


def print_baselines() -> int:
    for fixture in FIXTURES:
        print("%s:" % fixture)
        for line in BASELINES[fixture]:
            print("  " + line)
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="execute the real renderer and check all fixtures")
    mode.add_argument("--self-test", action="store_true",
                      help="prove the gate can go red with canaries")
    mode.add_argument("--print-baselines", action="store_true",
                      help="show the embedded real-render baselines")
    parser.add_argument("--renderer", type=Path, default=hwg.default_renderer(),
                        help="host_webapp_render.elf (default: discovered)")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        if args.print_baselines:
            return print_baselines()
        return run_gate(args.renderer.resolve())
    except GateError as exc:
        print("browser gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
