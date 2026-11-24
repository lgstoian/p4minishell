#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_tui_gate.py - host render gate for the new TUI cell-buffer scenes.

A hardware-free pre-gate (never hardware sign-off): it compiles the ONE
production cell renderer (components/tui/tui_frame.h), the ONE DOS palette
(components/ansi/ansi.c) and the ONE CSS colour parser
(components/markdown/css_color.c) with a plain C compiler and drives them over
scene fixtures in tools/host/fixtures/tui/. No second renderer, sampler or
colour parser is introduced.

The harness prints a `tui dump /a`-style report per scene: normalised text
rows, `NNN@start-end:fg` colour runs, and the dirty rect before/after the
single commit. Baselines live in fixtures/tui/baseline/ and compare exactly.

Usage:
    python tools/host_tui_gate.py              # --check (default)
    python tools/host_tui_gate.py --check
    python tools/host_tui_gate.py --update     # regenerate baselines
    python tools/host_tui_gate.py --self-test  # red canary for the gate
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FIXTURES = TOOLS / "host" / "fixtures" / "tui"
BASELINE = FIXTURES / "baseline"
HARNESS = FIXTURES / "host_tui_render.c"
STUBS = FIXTURES / "stubs"
BUILD = ROOT / "test" / "host" / "build-wsl"

SOURCES = [
    HARNESS,
    ROOT / "components" / "ansi" / "ansi.c",
    ROOT / "components" / "markdown" / "css_color.c",
]
INCLUDES = [
    STUBS,
    ROOT / "components" / "tui",
    ROOT / "components" / "ansi",
    ROOT / "components" / "markdown",
    ROOT / "components" / "gfx",
    ROOT,
]

BEGIN = "tui.scene.begin "
DUMP_HEAD = "tui.scene.dump "
DIRTY = "tui.scene.dirty "
CLEAN = "tui.scene.clean "
END = "tui.scene.end "
ROW_RE = re.compile(r"^(\d{3})\|(.*)$")
RUN_RE = re.compile(r"^(\d{3})@(.*)$")
RUN_ITEM_RE = re.compile(r"^\d+-\d+:\d+$")
RECT_RE = re.compile(r"^any=(yes|no) rect=(\d+),(\d+)-(\d+),(\d+)$")
CLEAN_RE = re.compile(r"^any=(yes|no)$")


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


def wsl_path(path: Path) -> str:
    """Map a Windows drive path to its WSL mount path."""
    text = str(path.resolve()).replace("\\", "/")
    if len(text) >= 2 and text[1] == ":":
        return "/mnt/" + text[0].lower() + text[2:]
    return text


class Compiler:
    """A gcc invocation, native or through WSL, with translated paths."""

    def __init__(self, native: bool, program: str) -> None:
        self.native = native
        self.program = program

    def arg(self, path: Path) -> str:
        return str(path) if self.native else wsl_path(path)

    @staticmethod
    def discover() -> "Compiler":
        found = shutil.which("gcc") or shutil.which("cc")
        if found:
            return Compiler(True, found)
        if sys.platform == "win32":
            try:
                probe = subprocess.run(
                    ["wsl", "-e", "sh", "-lc", "command -v gcc"],
                    capture_output=True, text=True, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                probe = None
            if probe is not None and probe.returncode == 0 and probe.stdout.strip():
                return Compiler(False, "gcc")
        raise GateError(
            "no C compiler: install gcc (or WSL2 Ubuntu with gcc); "
            "a host gate without a compiler cannot prove a render")

    def _shell(self, parts: List[str], timeout: int) -> subprocess.CompletedProcess:
        if self.native:
            return subprocess.run(parts, capture_output=True, text=True,
                                  timeout=timeout)
        script = " ".join("'" + a.replace("'", "'\\''") + "'" for a in parts)
        return subprocess.run(["wsl", "-e", "sh", "-lc", script],
                              capture_output=True, text=True, timeout=timeout)

    def run(self, args: List[str]) -> subprocess.CompletedProcess:
        """Compile: the first part is the compiler itself."""
        return self._shell([self.program] + args, 120)

    def execute(self, exe: Path, args: List[str]) -> subprocess.CompletedProcess:
        """Run the built harness directly (not through the compiler)."""
        return self._shell([self.arg(exe)] + args, 180)


def build(compiler: Compiler, out: Path) -> None:
    if not HARNESS.is_file():
        raise GateError("host TUI harness is missing: %s" % HARNESS)
    for src in SOURCES:
        if not src.is_file():
            raise GateError("production source is missing: %s" % src)
    args = ["-std=c11", "-Wall", "-Wextra"]
    args += ["-I" + compiler.arg(p) for p in INCLUDES]
    args += ["-o", compiler.arg(out)]
    args += [compiler.arg(p) for p in SOURCES]
    result = compiler.run(args)
    if result.returncode != 0:
        raise GateError("harness build failed:\n%s%s" %
                        (result.stdout[-2000:], result.stderr[-2000:]))


def scene_files() -> List[Path]:
    files = sorted(p for p in FIXTURES.glob("*.txt") if p.is_file())
    if not files:
        raise GateError("no TUI scene fixtures under %s" % FIXTURES)
    return files


def render(compiler: Compiler, exe: Path) -> str:
    scenes = scene_files()
    args = [compiler.arg(p) for p in scenes]
    result = compiler.execute(exe, args)
    if result.returncode != 0:
        raise GateError("harness run failed (%d):\n%s%s" %
                        (result.returncode, result.stdout[-2000:],
                         result.stderr[-2000:]))
    return result.stdout.replace("\r\n", "\n").replace("\r", "\n")


def scene_name(line: str, prefix: str) -> Optional[str]:
    """Read the `name=<scene>` field that starts a begin/end marker."""
    tokens = line[len(prefix):].split()
    if not tokens or not tokens[0].startswith("name="):
        return None
    name = tokens[0][len("name="):]
    return name if name else None


def parse_emission(raw: str) -> Tuple[List[str], Dict[str, int]]:
    """Validate the report and return canonical lines plus assertion counts."""
    canonical: List[str] = []
    open_scene: Optional[str] = None
    rows: Dict[int, str] = {}
    runs: Dict[int, List[str]] = {}
    counts = {"scenes": 0, "rows": 0, "runs": 0, "checks": 0}
    scene_checks: Dict[str, set] = {}

    for raw_line in raw.split("\n"):
        line = raw_line.rstrip()
        if not line:
            continue
        if line.startswith(BEGIN):
            if open_scene is not None:
                raise GateError("nested scene begin: %s" % line)
            open_scene = scene_name(line, BEGIN)
            if open_scene is None:
                raise GateError("scene begin without a name")
            scene_checks[open_scene] = set()
            canonical.append(line)
            continue
        if line.startswith(END):
            if open_scene is None or scene_name(line, END) != open_scene:
                raise GateError("unmatched scene end: %s" % line)
            if counts["rows"] == 0 or counts["runs"] == 0:
                raise GateError("scene %s emitted no rows or no colour runs"
                                % open_scene)
            seen = scene_checks[open_scene]
            if seen != {"dirty", "clean"}:
                raise GateError("scene %s is missing a dirty/clean commit "
                                "check (got %s)" % (open_scene, sorted(seen)))
            canonical.append(line)
            counts["scenes"] += 1
            open_scene = None
            rows.clear()
            runs.clear()
            continue
        if open_scene is None:
            raise GateError("emission outside a scene: %s" % line)
        if line.startswith(DUMP_HEAD):
            canonical.append(line)
            continue
        if line.startswith(DIRTY):
            m = RECT_RE.match(line[len(DIRTY):])
            if m is None:
                raise GateError("malformed dirty rect: %s" % line)
            if m.group(1) != "yes":
                raise GateError("scene %s has no dirty cells" % open_scene)
            rect = [int(v) for v in m.groups()[1:]]
            if rect[0] > rect[2] or rect[1] > rect[3] or rect[0] < 1:
                raise GateError("invalid dirty rect: %s" % line)
            canonical.append(line)
            counts["checks"] += 1
            scene_checks[open_scene].add("dirty")
            continue
        if line.startswith(CLEAN):
            m = CLEAN_RE.match(line[len(CLEAN):])
            if m is None or m.group(1) != "no":
                raise GateError("frame not clean after commit: %s" % line)
            canonical.append(line)
            counts["checks"] += 1
            scene_checks[open_scene].add("clean")
            continue
        m = ROW_RE.match(line)
        if m:
            index = int(m.group(1))
            if index in rows:
                raise GateError("duplicate row %03d" % index)
            rows[index] = m.group(2)
            canonical.append(line)
            counts["rows"] += 1
            continue
        m = RUN_RE.match(line)
        if m:
            index = int(m.group(1))
            items = m.group(2).strip().split()
            if not items or any(RUN_ITEM_RE.match(i) is None for i in items):
                raise GateError("malformed colour runs: %s" % line)
            if index in runs:
                raise GateError("duplicate run row %03d" % index)
            runs[index] = items
            canonical.append(line)
            counts["runs"] += 1
            continue
        raise GateError("unrecognised emission line: %s" % line)

    if open_scene is not None:
        raise GateError("scene %s has no end marker" % open_scene)
    if counts["scenes"] == 0:
        raise GateError("vacuous emission: no complete scene")
    if counts["runs"] == 0:
        raise GateError("vacuous emission: no colour runs")
    return canonical, counts


def baseline_path(scene: Path) -> Path:
    return BASELINE / scene.name


def diff_lines(expected: List[str], actual: List[str], label: str) -> List[str]:
    """Return a bounded, readable diff (empty means equal)."""
    if expected == actual:
        return []
    out = ["%s: baseline mismatch" % label]
    for i in range(max(len(expected), len(actual))):
        e = expected[i] if i < len(expected) else "<missing>"
        a = actual[i] if i < len(actual) else "<missing>"
        if e != a:
            out.append("  line %d: expected %r, got %r" % (i + 1, e, a))
            if len(out) >= 9:
                out.append("  ... (truncated)")
                break
    return out


def split_by_scene(lines: List[str]) -> Dict[str, List[str]]:
    result: Dict[str, List[str]] = {}
    current: Optional[str] = None
    for line in lines:
        if line.startswith(BEGIN):
            current = scene_name(line, BEGIN)
            if current is None:
                continue
            result[current] = [line]
        elif current is not None:
            result[current].append(line)
    return result


def self_test() -> int:
    """Red canaries: the gate must go red on bad emissions AND bad baselines.

    A guard that was only ever seen green is not a guard. The parse canaries
    below never touch a fixture or a baseline: they run synthetic report
    strings through the same parser ``--check`` uses.
    """
    sample = ["tui.scene.begin name=x cols=2 rows=1",
              "tui.scene.dump name=x", "001|AB", "001@1-2:7",
              "tui.scene.dirty any=yes rect=1,1-2,1",
              "tui.scene.clean any=no", "tui.scene.end name=x"]
    checks: List[Tuple[str, bool]] = []

    def must_fail(name: str, lines) -> None:
        try:
            parse_emission("\n".join(lines) + "\n")
            ok = False
        except GateError:
            ok = True
        checks.append((name, ok))

    try:
        canonical, counts = parse_emission("\n".join(sample) + "\n")
        checks.append(("clean emission accepted", counts["scenes"] == 1 and
                       counts["rows"] == 1 and counts["runs"] == 1 and
                       counts["checks"] == 2 and len(canonical) == len(sample)))
    except GateError as exc:
        print("  clean emission unexpectedly failed: %s" % exc)
        checks.append(("clean emission accepted", False))

    must_fail("no rows rejected",
              [line for line in sample if not line.startswith("001|")])
    must_fail("no colour runs rejected",
              [line for line in sample if not line.startswith("001@")])
    must_fail("no dirty rect rejected",
              [line for line in sample if not line.startswith(DIRTY)])
    must_fail("uncommitted frame rejected",
              [line for line in sample if not line.startswith(CLEAN)])
    must_fail("unmatched scene rejected", sample[:-1])
    must_fail("empty dirty rect rejected",
              [line.replace("any=yes", "any=no") for line in sample])
    must_fail("frame not clean after commit rejected",
              [line.replace(CLEAN + "any=no", CLEAN + "any=yes")
               for line in sample])

    tampered = list(sample)
    tampered[2] = "001|A!"
    checks.append(("tampered baseline rejected",
                   diff_lines(tampered, sample, "canary") != []))
    dropped = [line for line in sample if not line.startswith("001@")]
    checks.append(("dropped colour run rejected",
                   diff_lines(dropped, sample, "canary") != []))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("host_tui_gate: self-test %s (%d canaries)"
          % ("OK" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main() -> int:
    argv = sys.argv[1:]
    update = "--update" in argv
    self_test_flag = "--self-test" in argv
    if self_test_flag:
        return self_test()

    try:
        compiler = Compiler.discover()
        tmp = Path(tempfile.mkdtemp(prefix="host_tui_gate_"))
        exe = tmp / "host_tui_render"
        build(compiler, exe)
        canonical, counts = parse_emission(render(compiler, exe))
        by_scene = split_by_scene(canonical)

        if update:
            BASELINE.mkdir(parents=True, exist_ok=True)
            for scene, lines in by_scene.items():
                (BASELINE / (scene + ".txt")).write_text(
                    "\n".join(lines) + "\n", encoding="utf-8", newline="\n")
            print("host_tui_gate: updated %d baseline(s)" % len(by_scene))
            return 0

        problems: List[str] = []
        for scene, lines in sorted(by_scene.items()):
            path = baseline_path(Path(scene + ".txt"))
            if not path.is_file():
                problems.append("missing baseline: %s" % path.relative_to(ROOT))
                continue
            expected = path.read_text(encoding="utf-8").splitlines()
            problems.extend(diff_lines(expected, lines, scene))
        for path in sorted(BASELINE.glob("*.txt")):
            if path.stem not in by_scene:
                problems.append("stale baseline: %s" % path.relative_to(ROOT))

        print("host_tui_gate: %d scene(s), %d row(s), %d run(s), %d check(s)"
              % (counts["scenes"], counts["rows"], counts["runs"],
                 counts["checks"]))
        for problem in problems:
            print("GATE FAIL " + problem)
        ok = not problems and counts["checks"] >= counts["scenes"]
        print("HOST TUI GATE %s" % ("OK" if ok else "FAIL"))
        return 0 if ok else 1
    except GateError as exc:
        print("HOST TUI GATE FAIL: %s" % exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
