#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_batch_run.py - host-only .bat execution gate (pre-gate, never sign-off).

The gate drives ``host_batch_run.elf`` once per fixture. That standalone
harness links the firmware's ONE batch interpreter (``components/batch/batch.c``)
and a thin execute_command router, so a real ``.bat`` app runs through the real
verbs on the host. There is no second command language, parser or evaluator in
this gate.

Fixtures and text baselines live under ``tools/host/fixtures/batch/``. The ELF
is discovered under the repository build directories; when it is a POSIX
binary on Windows it is run through WSL, with Windows paths converted to their
``/mnt/<drive>`` form. No absolute executable path is hardcoded.

A pass says the host interpreter output is stable. It says nothing about a
board: harness.md still governs hardware sign-off.

Usage:
    python tools/host_batch_run.py                 # --check, all committed fixtures
    python tools/host_batch_run.py --fixture HELLO --check
    python tools/host_batch_run.py --update
    python tools/host_batch_run.py --self-test
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
FIXTURES = TOOLS / "host" / "fixtures" / "batch"
BASELINE = FIXTURES / "baseline"
ELF_NAME = "host_batch_run.elf"

BASELINE_TAG = "batch-baseline-v1"
BEGIN = "=== BATCH TRANSCRIPT ==="
DONE = "=== BATCH DONE ==="
ERRORLEVEL_RE = re.compile(r"^ERRORLEVEL\s+(-?\d+)$")
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
SD_PREFIX = "/sdcard"
DONE_MARKER = "[M-HOST-BATCH-DONE]"
RUN_TIMEOUT = 60
MAX_DIFF_LINES = 8

# `--check` with no explicit fixture drives the whole committed suite. The
# apps gate imports this module but always names its own fixtures, so its
# behaviour is unchanged.
DEFAULT_FIXTURES = ("HELLO", "FEATURES", "REGISTRY")

# Lines that must never appear in an honest, fully executed fixture. The three
# additive entries are the red canaries for the registry / library / control
# surfaces: a fake registry write, a duplicate library path and a swallowed
# ERRORLEVEL are all dishonest runs, never something a baseline may bless.
FORBIDDEN = (
    "batch: unsupported command",
    "batch: command pipeline is not available",
    "could not be executed",
    "Unknown command:",
    "The system cannot find",
    "batch file not found",
    "not a directory",
    "reg: fake write",
    "lib: duplicate library path",
    "errorlevel swallowed",
)


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


@dataclass
class RunResult:
    fixture: str
    transcript: List[str] = field(default_factory=list)
    errorlevel: int = 0
    assertions: int = 0

    @property
    def canonical(self) -> List[str]:
        return list(self.transcript) + ["ERRORLEVEL %d" % self.errorlevel]


def discover_elf(explicit: Optional[Path]) -> Path:
    """Find the runner in the repo's host build dirs, preferring build-wsl.

    Deliberately scoped to the two known host build parents instead of walking
    the whole tree: on WSL the repo lives under /mnt/d and a recursive glob over
    .git/build artifacts takes minutes.
    """
    if explicit is not None:
        path = explicit
    else:
        patterns = (
            "test/host/build-*/" + ELF_NAME,
            "tools/host/build-*/" + ELF_NAME,
            "build-*/" + ELF_NAME,
        )
        found = sorted({p for pattern in patterns for p in ROOT.glob(pattern)})
        if not found:
            raise GateError(
                "%s not found in a host build directory under %s (build test/host first)"
                % (ELF_NAME, ROOT)
            )

        def preference(path: Path) -> Tuple[int, str]:
            parts = {part.lower() for part in path.parts}
            if "build-wsl" in parts:
                return (0, str(path))
            if "build-win" in parts:
                return (1, str(path))
            return (2, str(path))

        path = min(found, key=preference)
    if not path.is_file():
        raise GateError("runner is missing: %s" % path)
    return path.resolve()


def is_posix_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError as exc:
        raise GateError("cannot read %s: %s" % (path, exc)) from exc


def to_wsl_path(path: Path) -> str:
    """Turn a Windows path into the POSIX path WSL sees (no hardcoded drive)."""
    text = str(path.resolve()).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/(.*)$", text)
    if match:
        return "/mnt/%s/%s" % (match.group(1).lower(), match.group(2))
    return text


def run_elf(elf: Path, root: Path, fixture: str) -> str:
    """Run the harness, routing a POSIX ELF through WSL when on Windows."""
    script = SD_PREFIX + "/" + fixture + ".BAT"
    uses_wsl = os.name == "nt" and is_posix_elf(elf)
    if uses_wsl:
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if wsl is None:
            raise GateError("ELF is POSIX but wsl.exe is not available")
        root_arg = to_wsl_path(root)
        elf_arg = to_wsl_path(elf)
        command = [wsl, "--exec", elf_arg, root_arg, script]
        env_root = root_arg
        cwd = None
    else:
        command = [str(elf), str(root.resolve()), script]
        env_root = str(root.resolve())
        cwd = str(ROOT)

    env = dict(os.environ)
    env["P4_HOST_SD_ROOT"] = env_root
    try:
        proc = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                              text=True, timeout=RUN_TIMEOUT)
    except OSError as exc:
        raise GateError("cannot run %s here (%s); use a POSIX host" % (elf, exc)) from exc
    except subprocess.TimeoutExpired as exc:
        raise GateError("runner timed out after %ds on %s" % (RUN_TIMEOUT, fixture)) from exc

    if proc.returncode != 0:
        raise GateError("runner exited %d on %s: %s" % (
            proc.returncode, fixture, (proc.stderr or proc.stdout)[-400:]))
    return proc.stdout


def parse_emission(raw: str, fixture: str) -> RunResult:
    """Normalise harness output and reject stale, empty or dishonest reports."""
    text = ANSI_RE.sub("", raw.replace("\r\n", "\n").replace("\r", "\n"))
    lines = [line.rstrip() for line in text.split("\n")]

    if lines.count(BEGIN) != 1 or lines.count(DONE) != 1:
        raise GateError("stale markers: expected one %r and one %r" % (BEGIN, DONE))
    begin_at = lines.index(BEGIN)
    done_at = lines.index(DONE)
    if done_at <= begin_at:
        raise GateError("stale markers: %s must follow %s" % (DONE, BEGIN))

    transcript = [line for line in lines[begin_at + 1:done_at] if line.strip()]
    errorlevel = None
    for line in lines[done_at + 1:]:
        match = ERRORLEVEL_RE.match(line.strip())
        if match is not None:
            errorlevel = int(match.group(1))
    if errorlevel is None:
        raise GateError("missing ERRORLEVEL line after %s" % DONE)
    if not transcript:
        raise GateError("zero assertions: no transcript lines emitted for %s" % fixture)

    flat = "\n".join(transcript)
    for needle in FORBIDDEN:
        if needle.lower() in flat.lower():
            raise GateError("dishonest/incomplete run for %s: found %r" % (fixture, needle))
    if DONE_MARKER not in flat:
        raise GateError("fixture %s never emitted %s" % (fixture, DONE_MARKER))

    return RunResult(fixture=fixture, transcript=transcript,
                     errorlevel=errorlevel,
                     assertions=len(transcript) + 1)


def write_baseline(path: Path, fixture: str, result: RunResult) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    body = ["# %s fixture=%s" % (BASELINE_TAG, fixture)] + result.canonical
    path.write_text("\n".join(body) + "\n", encoding="utf-8")


def read_baseline(path: Path, fixture: str) -> List[str]:
    if not path.is_file():
        raise GateError("missing baseline %s (run with --update once the ELF is built)" % path)
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or not lines[0].startswith("# " + BASELINE_TAG):
        raise GateError("stale baseline marker in %s (expected %s)" % (path, BASELINE_TAG))
    if _kv(lines[0]).get("fixture") != fixture:
        raise GateError("stale baseline marker in %s (fixture mismatch)" % path)
    return [line.rstrip() for line in lines[1:] if line.strip()]


_KV_RE = re.compile(r"(\w+)=(\S+)")


def _kv(line: str) -> Dict[str, str]:
    return dict(_KV_RE.findall(line))


def compare_lines(actual: Sequence[str], expected: Sequence[str],
                  fixture: str) -> None:
    if list(actual) == list(expected):
        return
    diffs = []
    for index in range(max(len(actual), len(expected))):
        got = actual[index] if index < len(actual) else "<missing>"
        want = expected[index] if index < len(expected) else "<missing>"
        if got != want:
            diffs.append("  line %d: baseline=%r emitted=%r" % (index + 1, want, got))
        if len(diffs) >= MAX_DIFF_LINES:
            break
    raise GateError("text diff for %s (%d emitted, %d baselined):\n%s" % (
        fixture, len(actual), len(expected), "\n".join(diffs)))


def evaluate(raw: str, fixture: str, baseline_dir: Path, update: bool) -> Tuple[str, RunResult]:
    result = parse_emission(raw, fixture)
    if result.assertions <= 0:
        raise GateError("zero assertions for %s" % fixture)
    base = baseline_dir / (fixture + ".txt")
    if update:
        write_baseline(base, fixture, result)
        return "updated %s" % base, result
    compare_lines(result.canonical, read_baseline(base, fixture), fixture)
    return "matched %s" % base, result


def run_gate(fixture: str, root: Path, baseline_dir: Path,
             elf: Optional[Path], update: bool) -> int:
    fixture_file = root / (fixture + ".BAT")
    if not fixture_file.is_file():
        raise GateError("fixture is missing: %s" % fixture_file)
    if not root.is_dir():
        raise GateError("fixture root is not a directory: %s" % root)
    runner = discover_elf(elf)
    raw = run_elf(runner, root, fixture)
    note, result = evaluate(raw, fixture, baseline_dir, update)
    print("  " + note)
    print("  %s: %d transcript line(s), ERRORLEVEL %d, %d assertion(s)" % (
        fixture, len(result.transcript), result.errorlevel, result.assertions))
    print("batch gate %s: 1 fixture, %d assertion(s) -> OK" % (
        "UPDATE" if update else "CHECK", result.assertions))
    return 0


def run_suite(fixtures: Sequence[str], root: Path, baseline_dir: Path,
              elf: Optional[Path], update: bool) -> int:
    """Drive every committed fixture through the one real interpreter."""
    runner = discover_elf(elf)
    total = 0
    for fixture in fixtures:
        raw = run_elf(runner, root, fixture)
        note, result = evaluate(raw, fixture, baseline_dir, update)
        total += result.assertions
        print("  %s: %s" % (fixture, note))
        print("  %s: %d transcript line(s), ERRORLEVEL %d, %d assertion(s)" % (
            fixture, len(result.transcript), result.errorlevel, result.assertions))
    print("batch gate %s: %d fixture(s), %d assertion(s) -> OK" % (
        "UPDATE" if update else "CHECK", len(fixtures), total))
    return 0


def self_test() -> int:
    """Red canary: wrong baseline, missing marker, empty run, stale tag."""
    good = "\n".join([
        BEGIN,
        "NAME=p4",
        "N=3",
        "HOST NAME OK",
        DONE_MARKER,
        "after call N=3",
        DONE,
        "ERRORLEVEL 0",
    ]) + "\n"
    checks: List[Tuple[str, bool]] = []

    with tempfile.TemporaryDirectory(prefix="batch_gate_selftest_") as tmp:
        base = Path(tmp) / "baseline"
        result = parse_emission(good, "HELLO")
        write_baseline(base / "HELLO.txt", "HELLO", result)
        try:
            evaluate(good, "HELLO", base, update=False)
            checks.append(("clean report accepted", True))
        except GateError as exc:
            print("  clean report unexpectedly failed: %s" % exc)
            checks.append(("clean report accepted", False))

        wrong = good.replace("HOST NAME OK", "HOST NAME BAD")
        try:
            evaluate(wrong, "HELLO", base, update=False)
            checks.append(("wrong baseline rejected", False))
        except GateError:
            checks.append(("wrong baseline rejected", True))

        no_marker = good.replace(DONE_MARKER + "\n", "")
        try:
            evaluate(no_marker, "HELLO", base, update=False)
            checks.append(("missing done marker rejected", False))
        except GateError:
            checks.append(("missing done marker rejected", True))

        empty = "\n".join([BEGIN, DONE, "ERRORLEVEL 0"]) + "\n"
        try:
            evaluate(empty, "HELLO", base, update=False)
            checks.append(("zero assertions rejected", False))
        except GateError:
            checks.append(("zero assertions rejected", True))

        no_level = good.replace("ERRORLEVEL 0\n", "")
        try:
            evaluate(no_level, "HELLO", base, update=False)
            checks.append(("missing ERRORLEVEL rejected", False))
        except GateError:
            checks.append(("missing ERRORLEVEL rejected", True))

        dishonest = good.replace("HOST NAME OK", "batch: unsupported command on this host lane: draw")
        try:
            evaluate(dishonest, "HELLO", base, update=False)
            checks.append(("dishonest line rejected", False))
        except GateError:
            checks.append(("dishonest line rejected", True))

        # Additive red canaries: a fixture that reports a registry write it
        # never persisted, a library published twice under the same path, or
        # an ERRORLEVEL silently swallowed by cleanup must never go green.
        for label, fake_line in (
            ("fake registry write rejected", "reg: fake write"),
            ("duplicate library path rejected",
             "lib: duplicate library path sd:/APPS/APPLIB.BAT::probe"),
            ("swallowed errorlevel rejected", "errorlevel swallowed"),
        ):
            forged = good.replace("HOST NAME OK", fake_line)
            try:
                evaluate(forged, "HELLO", base, update=False)
                checks.append((label, False))
            except GateError:
                checks.append((label, True))

        stale = base / "HELLO.txt"
        stale.write_text(stale.read_text(encoding="utf-8").replace(
            "# " + BASELINE_TAG, "# batch-baseline-v0"), encoding="utf-8")
        try:
            evaluate(good, "HELLO", base, update=False)
            checks.append(("stale baseline marker rejected", False))
        except GateError:
            checks.append(("stale baseline marker rejected", True))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("batch gate self-test: %s (%d canaries)" % ("PASS" if ok else "FAIL",
                                                      len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="compare the fixture against its baseline (default)")
    mode.add_argument("--update", action="store_true",
                      help="rewrite the baseline from the current ELF output")
    parser.add_argument("--self-test", action="store_true",
                        help="run the red-canary self-test (no ELF needed)")
    parser.add_argument("--fixture", default=None,
                        help="single fixture name without extension (default: check all)")
    parser.add_argument("--root", type=Path, default=FIXTURES,
                        help="directory holding the .bat fixture")
    parser.add_argument("--baseline", type=Path, default=BASELINE,
                        help="directory holding committed baselines")
    parser.add_argument("--elf", type=Path, default=None,
                        help="explicit runner path (otherwise discovered)")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    try:
        if args.fixture is None:
            return run_suite(DEFAULT_FIXTURES, args.root.resolve(),
                             args.baseline.resolve(), args.elf, args.update)
        return run_gate(args.fixture, args.root.resolve(), args.baseline.resolve(),
                        args.elf, args.update)
    except GateError as exc:
        print("batch gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
