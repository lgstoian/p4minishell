#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_video_gate.py - board-free video/EPUB/archive core gate (pre-gate only).

The gate compiles ``tools/host/fixtures/video/host_video_probe.c`` together
with the SHIPPED cores: video_core.c, the real BMP half of imagefile (gfx.c),
the ONE cell sampler (imagefile_cells.h -> tui_frame.h), the real Expat TOC
parsers (epub_ncx.c / epub_nav.c) and the real USTAR machine/bounds core
(archive_format.c + strutil.c). The only host-only files are tiny allocator /
log / strlcpy shims in the fixtures' ``stubs/`` directory. PNG/JPEG fixtures
are deliberately absent (imgdec rides LVGL's vendored decoders); their single-
decoder wiring is checked statically instead of faking a decode.

It asserts: deterministic frame ordering, real BMP decode + distinct sampled
cells, the fps contract, bounded looping, EPUB TOC/spine structure, and archive
machine rows. ``--self-test`` proves the gate can go RED when a frame vanishes,
a loop lies, a TOC goes stale, or a second decoder appears.

A pass says nothing about a board: harness.md still governs hardware sign-off.
Usage:
    python tools/host_video_gate.py --check
    python tools/host_video_gate.py --self-test
"""
from __future__ import annotations

import argparse
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
FIXTURES = TOOLS / "host" / "fixtures" / "video"
STUBS = FIXTURES / "stubs"
COMPILER = Path(tempfile.gettempdir()) / "p4minishell" / "host_video_probe"

BEGIN = "video.report.begin"
END = "video.report.end"
FRAME_RE = re.compile(
    r"^video\.frame name=(\S+) state=(\S+)(?: native=(\d+)\.(\d+) "
    r"fit=(\d+)\.(\d+) cell=([0-9A-Fa-f]{2}))?$")
PLAN_RE = re.compile(r"^video\.plan count=(\d+) order=(\S+)$")
FPS_RE = re.compile(r"^video\.fps fps=(\d+) interval=(\d+)$")
TICK_RE = re.compile(
    r"^video\.tick step=(\d+) index=(\d+) played=(\d+) loops=(\d+) "
    r"advanced=([01]) budget=([01])$")
STATUS_RE = re.compile(r"^video\.status .+$")
EPUB_RE = re.compile(
    r"^video\.epub source=(\S+) count=(\d+) linked=(\d+) linear=(\d+) "
    r"targets=([-\d,]+)$")
EPUB_RE_NCX = re.compile(
    r"^video\.epub source=(\S+) count=(\d+) linked=(\d+) linear=(\d+) "
    r"target0=(-?\d+) target1=(-?\d+) stale=(-?\d+)$")
EPUB_RE_NAV = re.compile(
    r"^video\.epub source=(\S+) count=(\d+) linked=(\d+) linear=(\d+) "
    r"target1=(-?\d+)$")
ARCHIVE_RE = re.compile(r"^video\.archive row=(\S+)$")
ARCHIVE_SUMMARY_RE = re.compile(
    r"^video\.archive\.summary rc=(-?\d+) members=(\d+) safe=(\d+) "
    r"unsafe_rejected=(\d+)$")

EXPECTED_FRAMES = ("frame_000.bmp", "frame_001.bmp", "frame_002.bmp")
EXPECTED_ROWS = (
    "f|246|1700000000|-|0|FRAMES/frame_000.bmp",
    "f|246|1700000000|-|0|FRAMES/frame_001.bmp",
    "f|246|1700000000|-|0|FRAMES/frame_002.bmp",
)


class GateError(RuntimeError):
    """Anything that must make the gate red."""


@dataclass
class Report:
    lines: List[str] = field(default_factory=list)
    frames: List[Tuple[str, str, int, int, str]] = field(default_factory=list)
    plan: Optional[Tuple[int, List[str]]] = None
    fps: Dict[int, int] = field(default_factory=dict)
    ticks: List[Tuple[int, int, int, int, bool, bool]] = field(default_factory=list)
    epub: Dict[str, Dict[str, int]] = field(default_factory=dict)
    rows: List[str] = field(default_factory=list)
    summary: Optional[Tuple[int, int, int, int]] = None
    assertions: int = 0


def parse_report(raw: str) -> Report:
    """Normalise harness stdout; reject stale/empty reports."""
    text = raw.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.strip() for line in text.split("\n") if line.strip()]
    begin_at = [i for i, line in enumerate(lines) if line.startswith(BEGIN)]
    end_at = [i for i, line in enumerate(lines) if line == END]
    if len(begin_at) != 1 or len(end_at) != 1:
        raise GateError("stale markers: expected one %s and one %s" % (BEGIN, END))
    if end_at[0] <= begin_at[0]:
        raise GateError("stale markers: %s must follow %s" % (END, BEGIN))
    body = lines[begin_at[0] + 1:end_at[0]]
    report = Report(lines=body)

    for line in body:
        match = FRAME_RE.match(line)
        if match:
            if match.group(2) == "ok":
                report.frames.append((
                    match.group(1), match.group(2),
                    int(match.group(3)), int(match.group(4)),
                    match.group(7)))
            continue
        match = PLAN_RE.match(line)
        if match:
            report.plan = (int(match.group(1)), match.group(2).split(","))
            continue
        match = FPS_RE.match(line)
        if match:
            report.fps[int(match.group(1))] = int(match.group(2))
            continue
        match = TICK_RE.match(line)
        if match:
            report.ticks.append((int(match.group(1)), int(match.group(2)),
                                 int(match.group(3)), int(match.group(4)),
                                 match.group(5) == "1", match.group(6) == "1"))
            continue
        if STATUS_RE.match(line):
            continue
        match = EPUB_RE_NCX.match(line)
        if match:
            report.epub["ncx"] = {
                "count": int(match.group(2)), "linked": int(match.group(3)),
                "linear": int(match.group(4)), "target0": int(match.group(5)),
                "target1": int(match.group(6)), "stale": int(match.group(7)),
            }
            continue
        match = EPUB_RE_NAV.match(line)
        if match:
            report.epub["nav"] = {
                "count": int(match.group(2)), "linked": int(match.group(3)),
                "linear": int(match.group(4)), "target1": int(match.group(5)),
            }
            continue
        if EPUB_RE.match(line):
            raise GateError("malformed EPUB report line: %s" % line)
        match = ARCHIVE_RE.match(line)
        if match:
            report.rows.append(match.group(1))
            continue
        match = ARCHIVE_SUMMARY_RE.match(line)
        if match:
            report.summary = tuple(int(match.group(i)) for i in range(1, 5))
            continue
        raise GateError("unrecognised report line: %s" % line)

    report.assertions = (len(report.frames) + len(report.ticks) +
                         len(report.fps) + len(report.epub) +
                         len(report.rows) + len(report.plan or ()))
    if report.assertions == 0:
        raise GateError("zero assertions: harness emitted no evidence")
    return report


def evaluate_report(report: Report) -> List[str]:
    """All contract checks; a canary can mutate a report and expect red."""
    fails: List[str] = []
    names = [frame[0] for frame in report.frames]
    if tuple(names) != EXPECTED_FRAMES:
        fails.append("missing/out-of-order frame(s): %s" % names)
    cells = [frame[4] for frame in report.frames]
    if len(set(cells)) != len(cells):
        fails.append("decoded frames sampled to identical cells (no animation)")
    for frame in report.frames:
        if frame[2] != 8 or frame[3] != 8:
            fails.append("frame %s has wrong native size" % frame[0])
    if report.plan is None or report.plan[0] != len(EXPECTED_FRAMES):
        fails.append("frame plan is missing or under-counted")
    elif tuple(report.plan[1]) != EXPECTED_FRAMES:
        fails.append("frame plan order is wrong: %s" % report.plan[1])

    for fps, interval in ((1, 1000), (10, 100), (30, 33)):
        if report.fps.get(fps) != interval:
            fails.append("fps %d interval %s != %d"
                         % (fps, report.fps.get(fps), interval))

    # The report is emitted AFTER each advance: index 0 -> 1 -> 2 -> wrap 0.
    expected_steps = [(0, 1, 1, 0), (1, 2, 2, 0), (2, 0, 3, 1),
                      (3, 1, 4, 1), (4, 2, 5, 1)]
    if len(report.ticks) != len(expected_steps):
        fails.append("tick count %d != %d" % (len(report.ticks),
                                              len(expected_steps)))
    else:
        for tick, want in zip(report.ticks, expected_steps):
            if tick[:4] != want or not tick[4] or not tick[5]:
                fails.append("false loop / budget tick: %s (want %s)" % (tick, want))

    ncx = report.epub.get("ncx")
    if ncx != {"count": 3, "linked": 2, "linear": 2,
               "target0": 0, "target1": 1, "stale": -1}:
        fails.append("stale/incorrect NCX TOC structure: %s" % ncx)
    nav = report.epub.get("nav")
    if nav != {"count": 2, "linked": 2, "linear": 2, "target1": 1}:
        fails.append("stale/incorrect nav TOC structure: %s" % nav)

    if tuple(report.rows) != EXPECTED_ROWS:
        fails.append("archive listing rows differ: %s" % report.rows)
    if report.summary != (0, 3, 3, 3):
        fails.append("archive scan summary differs: %s" % (report.summary,))
    return fails


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def check_sources(sources: Optional[Dict[str, str]] = None) -> List[str]:
    """Static single-implementation checks; canaries mutate a source dict."""
    src = dict(sources) if sources is not None else {
        "video_c": _read(ROOT / "components" / "video" / "video.c"),
        "imagefile_c": _read(ROOT / "components" / "imagefile" / "imagefile.c"),
        "cells_h": _read(ROOT / "components" / "imagefile" / "imagefile_cells.h"),
        "command_c": _read(ROOT / "components" / "command" / "video_commands.c"),
        "archive_format_c": _read(ROOT / "components" / "archive" / "archive_format.c"),
        "archive_c": _read(ROOT / "components" / "archive" / "archive.c"),
        "epub_c": _read(ROOT / "components" / "epub" / "epub.c"),
        "epub_render_c": _read(ROOT / "components" / "epub" / "epub_render.c"),
    }
    fails: List[str] = []
    video = src["video_c"]

    for needle in ("imagefile_decode_fit(", "app_delay_ms(", "video_cursor_next("):
        if needle not in video:
            fails.append("video core no longer calls %s" % needle)
    for forbidden in ("lodepng", "tjpgd", "stb_image", "xTaskCreate",
                      "zip_open", "archive_create"):
        if forbidden in video.lower():
            fails.append("second decoder/scheduler/store in video core: %s"
                         % forbidden)
    if "imagefile_decode_mem(data, len, w, h, out)" not in src["imagefile_c"]:
        fails.append("imagefile no longer owns the one decode call")
    if "tui_sample_image_cells(" not in src["cells_h"]:
        fails.append("cell sampler no longer delegates to tui_frame.h")
    for needle in ("shell_command_video", "gfx_surface_blit_scaled",
                   "tui_draw_sprite", "return 2;"):
        if needle not in src["command_c"]:
            fails.append("video command facade missing %s" % needle)
    if "strutil_crc32_update(" not in src["archive_format_c"]:
        fails.append("archive CRC no longer delegates to strutil")
    if "0xEDB88320" in src["archive_c"] or "0xEDB88320" in src["archive_format_c"]:
        fails.append("second CRC-32 implementation in archive")
    for symbol in ("epub_render_chapter", "epub_extract_resource",
                   "epub_resolve_href"):
        definitions = sum(1 for text in (src["epub_c"], src["epub_render_c"])
                          if re.search(r"\b(?:esp_err_t|bool)\s+" + symbol +
                                       r"\s*\(", text))
        if definitions != 1:
            fails.append("%s has %d definition(s), expected one"
                         % (symbol, definitions))
    return fails


def to_posix(path: Path) -> str:
    text = str(path.resolve()).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/(.*)$", text)
    if match:
        return "/mnt/%s/%s" % (match.group(1).lower(), match.group(2))
    return text


def compile_harness() -> Path:
    """Compile the shipped cores + harness with gcc (WSL when on Windows)."""
    COMPILER.parent.mkdir(parents=True, exist_ok=True)
    includes = [FIXTURES / "stubs", ROOT,
                ROOT / "components" / "gfx", ROOT / "components" / "ansi",
                ROOT / "components" / "tui", ROOT / "components" / "imagefile",
                ROOT / "components" / "archive", ROOT / "components" / "epub" / "include",
                ROOT / "components" / "strutil" / "include",
                ROOT / "components" / "video"]
    sources = [FIXTURES / "host_video_probe.c",
               ROOT / "components" / "video" / "video_core.c",
               ROOT / "components" / "gfx" / "gfx.c",
               ROOT / "components" / "gfx" / "gfx_font.c",
               ROOT / "components" / "ansi" / "ansi.c",
               ROOT / "components" / "archive" / "archive_format.c",
               ROOT / "components" / "strutil" / "strutil.c",
               ROOT / "components" / "epub" / "epub_ncx.c",
               ROOT / "components" / "epub" / "epub_nav.c"]
    args = ["-std=c11", "-O0", "-Wall"]
    args += ["-I" + str(path) for path in includes]
    args += ["-include", str(STUBS / "strlcpy.h")]
    args += [str(path) for path in sources]
    args += ["-o", str(COMPILER), "-lm", "-lexpat"]

    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if wsl is None:
            raise GateError("wsl.exe is unavailable; build on a POSIX host")
        # Every path argument must be POSIX; leave flags and libraries alone.
        posix_args = []
        for arg in args:
            if arg.startswith("-I"):
                posix_args.append("-I" + to_posix(Path(arg[2:])))
            elif arg.startswith("-include"):
                posix_args.append(arg)
            elif arg.endswith((".c", ".h")) or arg == str(COMPILER) or arg.startswith("/mnt/"):
                posix_args.append(to_posix(Path(arg)))
            else:
                posix_args.append(arg)
        # The -include operand follows its flag and is also a path.
        posix_args[posix_args.index("-include") + 1] = to_posix(STUBS / "strlcpy.h")
        command = [wsl, "--exec", "gcc"] + posix_args
    else:
        command = ["gcc"] + args
    try:
        proc = subprocess.run(command, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise GateError("cannot compile host video probe: %s" % exc)
    if proc.returncode != 0:
        raise GateError("host video probe compile failed:\n%s"
                        % (proc.stderr or proc.stdout)[-1500:])
    return COMPILER


def run_probe(binary: Path) -> str:
    command = [str(binary), str(FIXTURES)]
    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if wsl is None:
            raise GateError("wsl.exe is unavailable; run on a POSIX host")
        command = [wsl, "--exec", to_posix(binary), to_posix(FIXTURES)]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise GateError("cannot run host video probe: %s" % exc)
    if proc.returncode != 0:
        raise GateError("probe exited %d: %s"
                        % (proc.returncode, (proc.stderr or proc.stdout)[-800:]))
    return proc.stdout


def run_check() -> int:
    source_fails = check_sources()
    for fail in source_fails:
        print("  [FAIL] %s" % fail)
    if source_fails:
        raise GateError("source contract has %d failure(s)" % len(source_fails))
    print("  source contract: one decoder, one sampler, one CRC, one TOC link")
    report = parse_report(run_probe(compile_harness()))
    fails = evaluate_report(report)
    for fail in fails:
        print("  [FAIL] %s" % fail)
    if fails:
        raise GateError("report contract has %d failure(s)" % len(fails))
    print("  %d frames decoded, %d ticks, %d EPUB documents, %d archive rows"
          % (len(report.frames), len(report.ticks), len(report.epub),
             len(report.rows)))
    print("host video gate CHECK: OK (%d assertions)" % report.assertions)
    return 0


def self_test() -> int:
    """Red canaries: prove each contract can fail loudly."""
    raw = run_probe(compile_harness())
    good = parse_report(raw)
    checks: List[Tuple[str, bool]] = [
        ("clean report accepted", not evaluate_report(good)),
        ("clean sources accepted", not check_sources()),
    ]

    def red_report(name: str, lines: List[str]) -> None:
        try:
            failures = evaluate_report(parse_report("\n".join(lines) + "\n"))
            checks.append((name, bool(failures)))
        except GateError as exc:
            checks.append((name, "markers" in str(exc) or "zero" in str(exc)))

    lines = list(good.lines)
    red_report("missing frame rejected",
               [line for line in lines if "frame_001.bmp" not in line])
    red_report("false loop rejected",
               [line.replace("video.tick step=3 index=0",
                             "video.tick step=3 index=2") for line in lines])
    red_report("stale TOC rejected",
               [line.replace("target1=1", "target1=99") for line in lines])
    red_report("zero assertions rejected", [])
    red_report("stale markers rejected", [line for line in lines if line != END])

    good_sources = {
        "video_c": _read(ROOT / "components" / "video" / "video.c"),
        "imagefile_c": _read(ROOT / "components" / "imagefile" / "imagefile.c"),
        "cells_h": _read(ROOT / "components" / "imagefile" / "imagefile_cells.h"),
        "command_c": _read(ROOT / "components" / "command" / "video_commands.c"),
        "archive_format_c": _read(ROOT / "components" / "archive" / "archive_format.c"),
        "archive_c": _read(ROOT / "components" / "archive" / "archive.c"),
        "epub_c": _read(ROOT / "components" / "epub" / "epub.c"),
        "epub_render_c": _read(ROOT / "components" / "epub" / "epub_render.c"),
    }
    duplicate = dict(good_sources)
    duplicate["video_c"] = good_sources["video_c"] + "\n/* lodepng_decode32 */\n"
    checks.append(("duplicate decoder rejected",
                   bool(check_sources(duplicate))))
    no_loop = dict(good_sources)
    no_loop["video_c"] = good_sources["video_c"].replace("app_delay_ms(",
                                                          "no_delay_ms(")
    checks.append(("missing shared delay rejected", bool(check_sources(no_loop))))
    no_sampler = dict(good_sources)
    no_sampler["cells_h"] = good_sources["cells_h"].replace(
        "tui_sample_image_cells(", "other_sample_image_cells(")
    checks.append(("second sampler rejected", bool(check_sources(no_sampler))))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("host video gate self-test: %s (%d canaries)"
          % ("PASS" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="run the gate (default)")
    mode.add_argument("--self-test", action="store_true", help="red-canary self-test")
    args = parser.parse_args(argv)
    try:
        return self_test() if args.self_test else run_check()
    except GateError as exc:
        print("host video gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
