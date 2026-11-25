#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_webapp_gate.py - host-only webapp render gate (pre-gate, never sign-off).

The gate discovers ``host_webapp_render.elf`` under ``test/host/build*``
fixture. That standalone harness parses and renders with the firmware's ONE DOM
engine (``components/markdown``), so there is no second renderer, parser or
colour model on the host lane. Its stdout is a ``tui dump /a``-style report:
normalised text rows, ``NNN@start-end:fg`` colour runs, and begin/end markers.

Baselines live next to the fixtures in ``baseline/``. Text baselines are exact;
an optional ``webapp.frame path=...`` line asks the gate to compare a PNG (or
BMP) frame as well, through the ONE pixel model in ``p4test.screenshot``.

The four DOS showcase fixtures (portal/dashboard/article/form) additionally
assert presentation on the renderer's own rows: CP437 boxed frames, a status
line, and no empty-box glyph or raw debug label. Legacy feature probes keep
their classic DOM/CSS/entity checks only.

A pass here says the host render is stable. It says nothing about a board:
harness.md still governs hardware sign-off.

Usage:
    python tools/host_webapp_gate.py              # --check (default)
    python tools/host_webapp_gate.py --check
    python tools/host_webapp_gate.py --update     # regenerate baselines
    python tools/host_webapp_gate.py --self-test  # red-canary the gate itself
"""
from __future__ import annotations

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FIXTURES = TOOLS / "host" / "fixtures" / "webapp"
BASELINE = FIXTURES / "baseline"


def default_renderer():
    """Find the host renderer ELF in any local build dir (no hardcoded path).

    The POSIX host target builds under ``test/host/build*`` on Linux/WSL and
    may be staged under ``tools/host/build*`` by host tools. The first existing
    ELF wins; ``--renderer`` still overrides it.
    """
    # Prefer the canonical WSL host lane: other agents can leave ad-hoc
    # build-* directories with an older ELF behind. A stale renderer that
    # predates a feature probe must never silently satisfy the gate.
    canonical = ROOT / "test" / "host" / "build-wsl" / "host_webapp_render.elf"
    if canonical.is_file():
        return canonical
    for parent in (ROOT / "test" / "host", TOOLS / "host"):
        for elf in sorted(parent.glob("build*/host_webapp_render.elf")):
            if elf.is_file():
                return elf
    return canonical


BASELINE_TAG = "webapp-baseline-v1"
BEGIN = "webapp.render.begin"
END = "webapp.render.end"
DUMP_HEAD = "tui.dump cols="
DUMP_END = "tui.dump.end"
CANVAS = "webapp.canvas "
IMAGE = "webapp.image "
FRAME = "webapp.frame "
META = "webapp."   # any other webapp.* line is a feature/capability report
ROW_RE = re.compile(r"^(\d{3})\|(.*)$")
RUN_RE = re.compile(r"^(\d{3})@(.*)$")
RUN_ITEM_RE = re.compile(r"^\d+-\d+:\d+$")
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
KV_RE = re.compile(r"(\w+)=(\S+)")

MAX_DIFF_LINES = 8
PNG_TOL = 16
PNG_MAX_RATIO = 0.01
PNG_MIN_INK = 16
RENDER_TIMEOUT = 60

# DOS-presentation assertions on the rows the ONE DOM renderer emitted. These
# are text checks on real renderer output, never a second render or colour
# model. A marker-only gate cannot tell a framed DOS page from a debug dump.
BOX_GLYPHS = ("┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "─", "│",
              "╔", "╗", "╚", "╝", "═", "║")
EMPTY_BOX_GLYPHS = ("□", "▯", "⎕", "\ufffd")
DEBUG_LABELS = ("webapp.", "dom_", "css_", "debug", "trace", "todo",
                "\\u00", "\\u25")
STATUS_TOKENS = ("STATUS", "READY", "ESC", "F1", "URL", "HTTP", "P4 NET")
MIN_BOXED_ROWS = 2
# Only the four showcase pages are DOS presentation proofs. Legacy feature
# probes (features.html, modern.html, recovery.html, hello.html in the
# self-test) stay on the classic DOM/CSS/entity assertions.
DOS_FIXTURES = frozenset(("portal.html", "dashboard.html", "article.html",
                          "form.html"))
WIKI_FIXTURE = "wiki_esp32.html"
# Image outcomes are reported by the ONE renderer through its optional image
# sink. The two image-bearing showcase fixtures must always report these kinds;
# dropping a decode silently is therefore a gate failure, not a quiet diff.
REQUIRED_IMAGE_KINDS = {
    "portal.html": frozenset(("background", "img")),
    "dashboard.html": frozenset(("canvas",)),
}
IMAGE_STATES = frozenset(("ok", "resized", "missing", "fallback"))
IMAGE_RE = re.compile(
    r"^webapp\.image kind=(\S+) src=(\S+) state=(\S+) "
    r"native=(\d+)x(\d+) fit=(\d+)x(\d+) alt=(.*)$")


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


@dataclass
class RenderResult:
    fixture: str
    lines: List[str] = field(default_factory=list)
    rows: Dict[int, str] = field(default_factory=dict)
    runs: Dict[int, List[str]] = field(default_factory=dict)
    frames: List[Path] = field(default_factory=list)
    images: List[str] = field(default_factory=list)
    assertions: int = 0


def _kv(line: str) -> Dict[str, str]:
    return dict(KV_RE.findall(line))


def parse_emission(raw: str, fixture: str, fixture_dir: Path) -> RenderResult:
    """Normalise harness stdout and reject a stale or vacuous report."""
    text = ANSI_RE.sub("", raw.replace("\r\n", "\n").replace("\r", "\n"))
    begin: Optional[str] = None
    end: Optional[str] = None
    dump_head: Optional[str] = None
    dump_end = False
    canonical: List[str] = []
    rows: Dict[int, str] = {}
    runs: Dict[int, List[str]] = {}
    frames: List[Path] = []
    images: List[str] = []
    canvas_lines = 0
    meta_lines = 0

    for line in text.split("\n"):
        line = line.rstrip()
        if line.startswith(BEGIN):
            begin = line
        if line.startswith(END):
            end = line
        if line.startswith(DUMP_HEAD):
            dump_head = line
        if line == DUMP_END:
            dump_end = True

        match = ROW_RE.match(line)
        if match:
            index = int(match.group(1))
            if index in rows:
                raise GateError("duplicate row marker %03d" % index)
            rows[index] = match.group(2)
            canonical.append(line)
            continue

        match = RUN_RE.match(line)
        if match:
            index = int(match.group(1))
            payload = match.group(2).strip()
            items = payload.split() if payload else []
            if any(not RUN_ITEM_RE.match(item) for item in items):
                raise GateError("malformed colour run on row %03d: %s"
                                % (index, payload))
            items.sort(key=lambda s: int(s.split("-", 1)[0]))
            if index in runs:
                raise GateError("duplicate run marker %03d" % index)
            runs[index] = items
            canonical.append("%03d@%s" % (index, " ".join(items)))
            continue

        if line.startswith(CANVAS):
            canvas_lines += 1
            canonical.append(line)
            continue

        if line.startswith(IMAGE):
            match = IMAGE_RE.match(line)

            if match is None:
                raise GateError("malformed image report: %s" % line)
            state = match.group(3)
            if state not in IMAGE_STATES:
                raise GateError("unknown image state %r in %s" % (state, line))
            images.append(line)
            meta_lines += 1
            canonical.append(line)
            continue

        if (line.startswith(META) and not line.startswith(BEGIN) and
                not line.startswith(END)):
            meta_lines += 1
            canonical.append(line)
            continue

        if line.startswith(FRAME):
            rest = line[len(FRAME):].strip()
            if rest.startswith("path="):
                rest = rest[5:]
            if not rest:
                raise GateError("webapp.frame line without a path")
            path = Path(rest)
            frames.append(path if path.is_absolute() else fixture_dir / path)
            canonical.append(FRAME + "path=" + path.name)
            continue

        if (line.startswith(BEGIN) or line.startswith(END) or
                line.startswith(DUMP_HEAD) or line == DUMP_END):
            canonical.append(line)

    missing = []
    if begin is None:
        missing.append(BEGIN)
    if end is None:
        missing.append(END)
    if dump_head is None:
        missing.append(DUMP_HEAD)
    if not dump_end:
        missing.append(DUMP_END)
    if missing:
        raise GateError("stale/missing marker(s): " + ", ".join(missing))

    begin_kv, end_kv = _kv(begin), _kv(end)
    if begin_kv.get("fixture") != fixture or end_kv.get("fixture") != fixture:
        raise GateError("stale marker: fixture mismatch (expected %s, got %s/%s)"
                        % (fixture, begin_kv.get("fixture"), end_kv.get("fixture")))
    try:
        declared_rows = int(end_kv["rows"])
        declared_runs = int(end_kv["runs"])
    except (KeyError, ValueError):
        raise GateError("stale marker: unreadable end counts: %s" % end)
    if declared_rows != len(rows) or declared_runs != len(runs):
        raise GateError("stale marker: end says rows=%d runs=%d, dump has %d/%d"
                        % (declared_rows, declared_runs, len(rows), len(runs)))
    if not rows or not runs:
        raise GateError("zero assertions: no rows or colour runs were emitted")
    if meta_lines == 0:
        raise GateError("zero assertions: no webapp.* feature report was emitted")

    return RenderResult(fixture=fixture, lines=canonical, rows=rows, runs=runs,
                        frames=frames, images=images,
                        assertions=len(rows) + len(runs) + canvas_lines + meta_lines)


def check_dos_presentation(result: RenderResult) -> List[str]:
    """Assert the rendered rows show a framed DOS page, not a debug transcript."""
    fails: List[str] = []
    boxed = [idx for idx, text in result.rows.items()
             if sum(glyph in text for glyph in BOX_GLYPHS) >= 2]
    if len(boxed) < MIN_BOXED_ROWS:
        fails.append("only %d boxed row(s), expected at least %d"
                     % (len(boxed), MIN_BOXED_ROWS))
    if not any(token in text.upper()
               for text in result.rows.values() for token in STATUS_TOKENS):
        fails.append("no status/footer token in any rendered row")
    if boxed and not any(idx in result.runs for idx in boxed):
        fails.append("boxed rows carry no DOS colour runs")
    for idx, text in sorted(result.rows.items()):
        for glyph in EMPTY_BOX_GLYPHS:
            if glyph in text:
                fails.append("row %03d has an empty-box glyph %r" % (idx, glyph))
        for label in DEBUG_LABELS:
            if label in text.lower():
                fails.append("row %03d has a raw debug label %r" % (idx, label))
    return fails


def check_image_reports(result: RenderResult) -> List[str]:
    """Assert image degradation is visible and honest, not a silent drop.

    This is a text check on the ONE renderer's own report, never a second
    decoder or a re-render: one outcome per image, an alt text for every failed
    decode, a cell aspect derived from the native size, and a required-kind
    check on the showcase fixtures so a missing image line goes red.
    """
    fails: List[str] = []
    kinds = set()

    for line in result.images:
        match = IMAGE_RE.match(line)
        if match is None:
            fails.append("malformed image report %r" % line)
            continue
        kind, _src, state = match.group(1), match.group(2), match.group(3)
        native_w, native_h = int(match.group(4)), int(match.group(5))
        fit_w, fit_h = int(match.group(6)), int(match.group(7))
        alt = match.group(8).strip()
        kinds.add(kind)

        if state in ("missing", "fallback"):
            if state == "missing" and kind == "img" and alt in ("", "-"):
                fails.append("missing image %r has no visible alt text" % line)
            if fit_w != 0 or fit_h != 0:
                fails.append("%s image reports a fitted size" % state)
            continue
        if native_w < 1 or native_h < 1 or fit_w < 1 or fit_h < 1:
            fails.append("decoded image has degenerate geometry: %r" % line)
            continue
        # Cells are roughly 2:1, so the correct fit ratio is
        # fit_w/fit_h ~= 2*native_w/native_h. Generous bounds accommodate the
        # integer clamping at 80x25; a mirrored or stretched image cannot hide.
        expected = (2.0 * native_w) / native_h
        actual = fit_w / fit_h
        ratio = actual / expected
        if ratio < 0.5 or ratio > 2.0:
            fails.append("wrong image aspect %r (fit %.2f, expected %.2f)"
                         % (line, actual, expected))

    for kind in REQUIRED_IMAGE_KINDS.get(result.fixture, ()):  # type: ignore[index]
        if kind not in kinds:
            fails.append("image kind %r was silently dropped" % kind)
    return fails


def check_wiki_block(result: RenderResult) -> List[str]:
    """The Wikipedia proof must show an infobox row and a decoded image.

    Both are checked on the renderer's own 25 rows / outcome report. A fixture
    that loses its infobox or whose image silently disappears must go red.
    """
    fails: List[str] = []
    if not any("Manufacturer" in text for text in result.rows.values()):
        fails.append("wiki infobox block is missing")
    if not any(line.startswith(IMAGE) and "kind=img " in line and
               " state=missing" not in line and " state=fallback" not in line
               for line in result.images):
        fails.append("wiki image block is missing")
    if not any("state=missing" in line and "alt=" in line
               and line.rstrip().endswith("-") is False
               for line in result.images):
        fails.append("wiki missing-image alt fallback is missing")
    return fails


def write_baseline(path: Path, fixture: str, result: RenderResult) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    body = ["# %s fixture=%s" % (BASELINE_TAG, fixture)] + result.lines
    path.write_text("\n".join(body) + "\n", encoding="utf-8")


def read_baseline(path: Path, fixture: str) -> List[str]:
    if not path.is_file():
        raise GateError("missing baseline %s (run with --update once the "
                        "renderer is built)" % path)
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or not lines[0].startswith("# " + BASELINE_TAG):
        raise GateError("stale baseline marker in %s (expected %s)"
                        % (path, BASELINE_TAG))
    if _kv(lines[0]).get("fixture") != fixture:
        raise GateError("stale baseline marker in %s (fixture mismatch)" % path)
    return [line.rstrip() for line in lines[1:] if line.strip()]


def compare_lines(actual: Sequence[str], expected: Sequence[str],
                  fixture: str) -> None:
    if list(actual) == list(expected):
        return
    diffs = []
    for i in range(max(len(actual), len(expected))):
        got = actual[i] if i < len(actual) else "<missing>"
        want = expected[i] if i < len(expected) else "<missing>"
        if got != want:
            diffs.append("  line %d: baseline=%r emitted=%r" % (i + 1, want, got))
        if len(diffs) >= MAX_DIFF_LINES:
            break
    raise GateError("text diff for %s (%d emitted, %d baselined):\n%s"
                    % (fixture, len(actual), len(expected), "\n".join(diffs)))


def _load_pixel_model():
    """Import the ONE host pixel model; never re-implement it here."""
    tools = str(ROOT / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    try:
        from p4test import screenshot, theme_guard, visual  # noqa: E402
    except Exception as exc:  # noqa: BLE001
        raise GateError("PNG comparison needs the p4test pixel model "
                        "(pyserial, numpy, Pillow): %s" % exc)
    return screenshot, theme_guard, visual


def _bmp_from_png(path: Path, screenshot):
    from PIL import Image

    with Image.open(path) as img:
        buf = io.BytesIO()
        img.convert("RGB").save(buf, format="BMP")
    return screenshot.parse_bmp(buf.getvalue())


def _bmp_from_frame(path: Path, screenshot):
    if not path.is_file():
        raise GateError("rendered frame is missing: %s" % path)
    if path.suffix.lower() == ".bmp":
        return screenshot.parse_bmp(path.read_bytes())
    try:
        return _bmp_from_png(path, screenshot)
    except Exception as exc:  # noqa: BLE001
        raise GateError("undecodable frame %s: %s" % (path, exc))


def compare_frame(frame: Path, baseline: Path) -> List[str]:
    """Compare a PNG/BMP frame using p4test.screenshot's pixel model."""
    screenshot, theme_guard, visual = _load_pixel_model()
    if not baseline.is_file():
        raise GateError("missing frame baseline %s (run with --update)" % baseline)
    got = _bmp_from_frame(frame, screenshot)
    want = _bmp_from_frame(baseline, screenshot)
    if (got.width, got.height) != (want.width, want.height):
        raise GateError("frame size diff: %s is %dx%d, baseline %s is %dx%d"
                        % (frame.name, got.width, got.height,
                           baseline.name, want.width, want.height))
    ratio = screenshot.diff_ratio(got, want, tol=PNG_TOL)
    if ratio > PNG_MAX_RATIO:
        raise GateError("frame pixel diff for %s: %.3f (limit %.3f)"
                        % (frame.name, ratio, PNG_MAX_RATIO))
    arr = visual.array(got)
    ink = int(visual.nonbg_mask(arr, visual.bg_color(arr)).sum())
    if ink < PNG_MIN_INK:
        raise GateError("frame %s is blank (%d non-background pixels)"
                        % (frame.name, ink))
    hits = theme_guard.scan_bmp(got)
    if hits:
        raise GateError("frame %s has LVGL default-theme pixels: %s"
                        % (frame.name, theme_guard.describe(hits)))
    return ["frame %s: %dx%d, diff=%.4f, ink=%d"
            % (frame.name, got.width, got.height, ratio, ink)]


def evaluate(raw: str, fixture: str, fixture_dir: Path, baseline_dir: Path,
             update: bool, compare_frames: bool = True) -> Tuple[List[str], RenderResult]:
    result = parse_emission(raw, fixture, fixture_dir)
    if result.assertions <= 0:
        raise GateError("zero assertions for %s" % fixture)
    image_fails = check_image_reports(result)
    if image_fails:
        raise GateError("image report for %s: %s"
                        % (fixture, "; ".join(image_fails)))
    if Path(fixture).name == WIKI_FIXTURE:
        wiki_fails = check_wiki_block(result)
        if wiki_fails:
            raise GateError("wiki block for %s: %s"
                            % (fixture, "; ".join(wiki_fails)))
    if Path(fixture).name in DOS_FIXTURES:
        presentation = check_dos_presentation(result)
        if presentation:
            raise GateError("DOS presentation for %s: %s"
                            % (fixture, "; ".join(presentation)))
    base = baseline_dir / (Path(fixture).stem + ".txt")

    if update:
        write_baseline(base, fixture, result)
        baseline_dir.mkdir(parents=True, exist_ok=True)
        for frame in result.frames:
            if not frame.is_file():
                raise GateError("rendered frame is missing: %s" % frame)
            (baseline_dir / frame.name).write_bytes(frame.read_bytes())
        return ["updated %s" % base], result

    compare_lines(result.lines, read_baseline(base, fixture), fixture)
    notes: List[str] = []
    if compare_frames:
        for frame in result.frames:
            notes.extend(compare_frame(frame, baseline_dir / frame.name))
    return notes, result


def _is_posix_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def _to_wsl_path(path: Path) -> str:
    """Turn a Windows path into the POSIX path WSL sees (no hardcoded drive)."""
    text = str(path.resolve()).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/(.*)$", text)
    if match:
        return "/mnt/%s/%s" % (match.group(1).lower(), match.group(2))
    return text


def run_renderer(renderer: Path, fixture: Path, cwd: Path) -> str:
    if not renderer.is_file():
        raise GateError("renderer %s is missing; build it before running "
                        "this gate" % renderer)
    command = [str(renderer), str(fixture)]
    workdir = str(cwd)
    # On Windows the host ELF is POSIX; route it through WSL (same convention
    # as tools/host_batch_run.py) instead of failing with WinError 193.
    if os.name == "nt" and _is_posix_elf(renderer):
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if wsl is None:
            raise GateError("renderer is a POSIX ELF but wsl.exe is unavailable")
        command = [wsl, "--exec", _to_wsl_path(renderer), _to_wsl_path(fixture)]
        workdir = None
    try:
        # The renderer emits UTF-8 (HTML entities decode to UTF-8). On a
        # Windows host the locale codec is cp1252, which would silently turn
        # `©` into `Â©` in the baseline and in every proof image; pin UTF-8.
        proc = subprocess.run(command, cwd=workdir, capture_output=True,
                              text=True, encoding="utf-8", errors="replace",
                              timeout=RENDER_TIMEOUT)
    except OSError as exc:
        raise GateError("cannot run %s here (%s); use a POSIX host" % (renderer, exc))
    except subprocess.TimeoutExpired:
        raise GateError("renderer timed out after %ds on %s"
                        % (RENDER_TIMEOUT, fixture.name))
    if proc.returncode != 0:
        raise GateError("renderer exited %d on %s: %s"
                        % (proc.returncode, fixture.name,
                           (proc.stderr or proc.stdout)[-400:]))
    return proc.stdout


def run_gate(fixtures_dir: Path, baseline_dir: Path, renderer: Path,
             update: bool, compare_frames: bool) -> int:
    fixtures = sorted(fixtures_dir.glob("*.html"))
    if not fixtures:
        raise GateError("no HTML fixtures in %s" % fixtures_dir)
    assertions = 0
    for fixture in fixtures:
        raw = run_renderer(renderer, fixture, fixtures_dir)
        notes, result = evaluate(raw, fixture.name, fixtures_dir, baseline_dir,
                                 update, compare_frames)
        assertions += result.assertions
        for note in notes:
            print("  " + note)
        print("  %s: %d rows, %d runs, %d frame(s), %d assertion(s)"
              % (fixture.name, len(result.rows), len(result.runs),
                 len(result.frames), result.assertions))
    mode = "UPDATE" if update else "CHECK"
    print("webapp gate %s: %d fixture(s), %d assertion(s) -> OK"
          % (mode, len(fixtures), assertions))
    return 0


def _png_canary() -> Optional[bool]:
    """Try the PNG path too; skip it cleanly when its host deps are absent."""
    try:
        screenshot, _, _ = _load_pixel_model()
        from PIL import Image, ImageDraw
    except Exception as exc:  # noqa: BLE001
        print("  PNG canary skipped: %s" % exc)
        return None
    with tempfile.TemporaryDirectory(prefix="webapp_gate_png_") as tmp:
        good_path = Path(tmp) / "good.png"
        bad_path = Path(tmp) / "bad.png"
        for path, colour in ((good_path, (200, 200, 200)),
                             (bad_path, (200, 40, 40))):
            img = Image.new("RGB", (32, 32), (20, 20, 20))
            ImageDraw.Draw(img).rectangle((4, 4, 20, 20), fill=colour)
            img.save(path)
        assert screenshot is not None
        same_ok = bool(compare_frame(good_path, good_path))
        try:
            compare_frame(bad_path, good_path)
            diff_rejected = False
        except GateError:
            diff_rejected = True
        return same_ok and diff_rejected


def self_test() -> int:
    """Prove the gate can go RED: wrong baseline, zero assertions, stale markers."""
    good = "\n".join([
        BEGIN + " fixture=hello.html cols=80 rows=25",
        DUMP_HEAD + " 80 from=1 to=25",
        "001|┌── P4 NET ──┐",
        "001@1-14:14",
        "002|│ STATUS READY │",
        "002@1-15:10",
        "003|└────────────┘",
        "003@1-14:7",
        DUMP_END,
        "webapp.meta semantic=2 controls=3 grids=1 canvases=1",
        "webapp.css id=card radius=3 shadow=1 gradient=1 opacity=128",
        "webapp.cascade h1_color=.win spec=0.1.0 imp=1 order=6 fg=14",
        "webapp.entity &amp;=26/5 &reg=AE/4 &#x41;=41/5",
        CANVAS + " id=c w=96 h=48 pixel0=ff5555ff",
        END + " fixture=hello.html rows=3 runs=3",
    ]) + "\n"
    checks: List[Tuple[str, bool]] = []

    with tempfile.TemporaryDirectory(prefix="webapp_gate_selftest_") as tmp:
        root = Path(tmp)
        base = root / "baseline"
        fixture_dir = root / "fixtures"
        fixture_dir.mkdir()
        (fixture_dir / "hello.html").write_text("<html></html>\n", encoding="utf-8")
        write_baseline(base / "hello.txt", "hello.html",
                       parse_emission(good, "hello.html", fixture_dir))
        try:
            evaluate(good, "hello.html", fixture_dir, base, update=False)
            checks.append(("clean report accepted", True))
        except GateError as exc:
            print("  clean report unexpectedly failed: %s" % exc)
            checks.append(("clean report accepted", False))

        wrong = good.replace("002|│ STATUS READY │", "002|│ STATXS READY │")
        try:
            evaluate(wrong, "hello.html", fixture_dir, base, update=False)
            checks.append(("wrong baseline rejected", False))
        except GateError:
            checks.append(("wrong baseline rejected", True))

        vacuous = "\n".join([
            BEGIN + " fixture=hello.html cols=80 rows=25",
            DUMP_HEAD + " 80 from=1 to=25",
            DUMP_END,
            END + " fixture=hello.html rows=0 runs=0",
        ]) + "\n"
        try:
            evaluate(vacuous, "hello.html", fixture_dir, base, update=False)
            checks.append(("zero assertions rejected", False))
        except GateError:
            checks.append(("zero assertions rejected", True))

        no_end = good.replace(END + " fixture=hello.html rows=3 runs=3\n", "")
        try:
            evaluate(no_end, "hello.html", fixture_dir, base, update=False)
            checks.append(("missing marker rejected", False))
        except GateError:
            checks.append(("missing marker rejected", True))

        no_meta = good.replace("webapp.meta semantic=2 controls=3 grids=1 canvases=1\n", "")
        try:
            evaluate(no_meta, "hello.html", fixture_dir, base, update=False)
            checks.append(("missing feature report rejected", False))
        except GateError:
            checks.append(("missing feature report rejected", True))

        bad_meta = good.replace("webapp.css id=card radius=3",
                                "webapp.css id=card radius=9")
        try:
            evaluate(bad_meta, "hello.html", fixture_dir, base, update=False)
            checks.append(("tampered feature report rejected", False))
        except GateError:
            checks.append(("tampered feature report rejected", True))

        # The cascade/entity lines are part of the same exact baseline: a
        # wrong specificity decision or entity mapping must go RED too.
        bad_cascade = good.replace("webapp.cascade h1_color=.win spec=0.1.0",
                                   "webapp.cascade h1_color=#hero spec=1.0.0")
        try:
            evaluate(bad_cascade, "hello.html", fixture_dir, base, update=False)
            checks.append(("tampered cascade decision rejected", False))
        except GateError:
            checks.append(("tampered cascade decision rejected", True))

        bad_entity = good.replace("&amp;=26/5", "&amp;=26/4")
        try:
            evaluate(bad_entity, "hello.html", fixture_dir, base, update=False)
            checks.append(("tampered entity mapping rejected", False))
        except GateError:
            checks.append(("tampered entity mapping rejected", True))

        # DOS-presentation canaries: text-only, no baseline ambiguity.
        for label, line, needle in (
            ("unboxed DOS report rejected",
             good.replace("┌── P4 NET ──┐", "P4 NET")
                 .replace("│ STATUS READY │", "STATUS READY")
                 .replace("└────────────┘", "END"), "boxed row"),
            ("missing status line rejected",
             good.replace("STATUS READY", "STATE UNKNOWN")
                 .replace("P4 NET", "XNET"), "status/footer"),
            ("empty-box glyph rejected",
             good.replace("│ STATUS READY │", "□ STATUS READY │"), "empty-box"),
            ("raw debug label rejected",
             good.replace("STATUS READY", "webapp.debug STATUS"), "debug label"),
        ):
            try:
                fails = check_dos_presentation(parse_emission(line, "hello.html",
                                                              fixture_dir))
                checks.append((label, any(needle in item for item in fails)))
            except GateError as exc:
                checks.append((label, False))
                print("  %s: %s" % (label, exc))

        # Image canaries: a missing decode must carry alt text, the cell fit
        # must preserve the native aspect, and a required image line may not
        # vanish. All three are checked on the renderer's own report.
        img_good = "\n".join([
            BEGIN + " fixture=portal.html cols=80 rows=25",
            DUMP_HEAD + " 80 from=1 to=25",
            "001|┌── P4 NET ──┐",
            "001@1-14:14",
            "002|│ STATUS READY │",
            "002@1-15:10",
            "003|└────────────┘",
            "003@1-14:7",
            DUMP_END,
            "webapp.image kind=background src=pixel.bmp state=ok "
            "native=32x4 fit=32x4 alt=-",
            "webapp.image kind=img src=pixel.bmp state=resized "
            "native=32x4 fit=32x2 alt=P4 badge",
            END + " fixture=portal.html rows=3 runs=3",
        ]) + "\n"
        (fixture_dir / "portal.html").write_text("<html></html>\n", encoding="utf-8")
        write_baseline(base / "portal.txt", "portal.html",
                       parse_emission(img_good, "portal.html", fixture_dir))
        try:
            evaluate(img_good, "portal.html", fixture_dir, base, update=False)
            checks.append(("honest image report accepted", True))
        except GateError as exc:
            print("  honest image report unexpectedly failed: %s" % exc)
            checks.append(("honest image report accepted", False))

        for label, line, needle in (
            ("missing image without alt rejected",
             img_good.replace("state=resized native=32x4 fit=32x2 alt=P4 badge",
                              "state=missing native=0x0 fit=0x0 alt=-"),
             "no visible alt text"),
            ("wrong image aspect rejected",
             img_good.replace("fit=32x2 alt=P4 badge", "fit=32x32 alt=P4 badge"),
             "wrong image aspect"),
            ("silently dropped image rejected",
             img_good.replace(
                 "webapp.image kind=img src=pixel.bmp state=resized "
                 "native=32x4 fit=32x2 alt=P4 badge\n", ""),
             "silently dropped"),
        ):
            try:
                parse = parse_emission(line, "portal.html", fixture_dir)
                fails = check_image_reports(parse)
                checks.append((label, any(needle in item for item in fails)))
            except GateError as exc:
                checks.append((label, needle in str(exc)))

        # Wikipedia proof canaries: the infobox row and the decoded image must
        # be present; dropping either must go red on the renderer's report.
        wiki_good = "\n".join([
            BEGIN + " fixture=wiki_esp32.html cols=80 rows=25",
            DUMP_HEAD + " 80 from=1 to=25",
            "001|ESP32",
            "001@1-5:0",
            "002|Manufacturer  Espressif Systems",
            "002@1-30:0",
            DUMP_END,
            "webapp.image kind=img src=esp32-module.bmp state=resized "
            "native=64x8 fit=32x2 alt=ESP-WROOM-32 module",
            "webapp.image kind=img src=esp32-die-missing.bmp state=missing "
            "native=0x0 fit=0x0 alt=ESP32 die shot unavailable",
            END + " fixture=wiki_esp32.html rows=2 runs=2",
        ]) + "\n"
        (fixture_dir / "wiki_esp32.html").write_text("<html></html>\n",
                                                      encoding="utf-8")
        write_baseline(base / "wiki_esp32.txt", "wiki_esp32.html",
                       parse_emission(wiki_good, "wiki_esp32.html", fixture_dir))
        try:
            evaluate(wiki_good, "wiki_esp32.html", fixture_dir, base,
                     update=False)
            checks.append(("wiki infobox + image report accepted", True))
        except GateError as exc:
            print("  wiki report unexpectedly failed: %s" % exc)
            checks.append(("wiki infobox + image report accepted", False))

        for label, line, needle in (
            ("wiki infobox dropped rejected",
             wiki_good.replace("002|Manufacturer  Espressif Systems\n", "")
                      .replace("002@1-30:0\n", "")
                      .replace(END + " fixture=wiki_esp32.html rows=2 runs=2",
                               END + " fixture=wiki_esp32.html rows=1 runs=1"),
             "infobox block is missing"),
            ("wiki image block dropped rejected",
             "\n".join(
                 [l for l in wiki_good.splitlines()
                  if not l.startswith(IMAGE) and not l.startswith(END)] +
                 ["webapp.meta semantic=1 controls=0 grids=0 canvases=0",
                  END + " fixture=wiki_esp32.html rows=2 runs=2"]) + "\n",
             "image block is missing"),
        ):
            try:
                fails = check_wiki_block(parse_emission(
                    line, "wiki_esp32.html", fixture_dir))
                checks.append((label, any(needle in item for item in fails)))
            except GateError as exc:
                checks.append((label, False))
                print("  %s: %s" % (label, exc))

        stale = base / "hello.txt"
        stale.write_text(stale.read_text(encoding="utf-8").replace(
            "# " + BASELINE_TAG, "# webapp-baseline-v0"), encoding="utf-8")
        try:
            evaluate(good, "hello.html", fixture_dir, base, update=False)
            checks.append(("stale baseline marker rejected", False))
        except GateError:
            checks.append(("stale baseline marker rejected", True))

    png = _png_canary()
    if png is not None:
        checks.append(("PNG canary (same accepted, diff rejected)", png))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("webapp gate self-test: %s (%d canaries)" % ("PASS" if ok else "FAIL",
                                                       len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="compare fixtures against baselines (default)")
    mode.add_argument("--update", action="store_true",
                      help="rewrite baselines from the current renderer output")
    parser.add_argument("--self-test", action="store_true",
                        help="run the red-canary self-test")
    parser.add_argument("--fixtures", type=Path, default=FIXTURES,
                        help="directory holding *.html fixtures")
    parser.add_argument("--baseline", type=Path, default=BASELINE,
                        help="directory holding committed baselines")
    parser.add_argument("--renderer", type=Path, default=default_renderer(),
                        help="standalone host render harness executable")
    parser.add_argument("--no-png", action="store_true",
                        help="compare text only (skip optional frame baselines)")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    try:
        return run_gate(args.fixtures.resolve(), args.baseline.resolve(),
                        args.renderer.resolve(), args.update, not args.no_png)
    except GateError as exc:
        print("webapp gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
