#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_apps_gate.py - host pre-gate for the enriched reference apps.

Every updated ``apps/**/*.BAT`` carries a visible ``[M-RICH-*]`` block that
uses the additive v1.6 surfaces. This gate does NOT reimplement the batch
language: it loads ``tools/host_batch_run.py`` as a library and drives each
host-safe fixture in ``tools/host/fixtures/apps/`` through the SAME real
interpreter ELF. Baselines are the exact transcript lines emitted by that
interpreter.

Hardware surfaces (gfx, TUI, modal, webapp, native) are not faked. They are
detected in the *real app source* and reported as honest SKIPs, with the
reason named; the fixture itself never contains an unsupported verb, so a
false green is impossible (``host_batch_run.parse_emission`` rejects the
``batch: unsupported command on this host lane`` string and any missing done
marker).

Usage:
    python tools/host_apps_gate.py                 # --check (default)
    python tools/host_apps_gate.py --update        # rewrite all baselines
    python tools/host_apps_gate.py --self-test     # red canaries, no ELF
"""
from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FIXTURES = TOOLS / "host" / "fixtures" / "apps"
BASELINE = FIXTURES / "baseline"

# (fixture name, real source path relative to ROOT, [M-RICH-*] tag)
APPS: Dict[str, Tuple[str, str]] = {
    "APPLIB": ("apps/APPLIB.BAT", "LIB"),
    "ADVENT": ("apps/adventure/ADVENT.BAT", "ADV"),
    "TCMD": ("apps/tcmd/TCMD.BAT", "TCMD"),
    "BOUNCE": ("apps/gfxdemo/BOUNCE.BAT", "BOUNCE"),
    "GFXTOOL": ("apps/gfxdemo/GFXTOOL.BAT", "GFXTOOL"),
    "PLOT": ("apps/gfxdemo/PLOT.BAT", "PLOT"),
    "MOOD": ("apps/mood/MOOD.BAT", "MOOD"),
    "MOODSVC": ("apps/mood/MOODSVC.BAT", "MOODSVC"),
    "CHAT": ("apps/chat/CHAT.BAT", "CHAT"),
    "DIAG": ("apps/diag/DIAG.BAT", "DIAG"),
    "ELITE": ("apps/elite/ELITE.BAT", "ELITE"),
    "NOTES": ("apps/notes/NOTES.BAT", "NOTE"),
    "PALMTOP": ("apps/palmtop/PALMTOP.BAT", "PALM"),
    "PICS": ("apps/pics/PICS.BAT", "PICS"),
    "SNAKE": ("apps/snake/SNAKE.BAT", "SNAKE"),
    "WEBAPP": ("apps/webapp/WEBAPP.BAT", "WEBAPP"),
    "WEB": ("apps/website/WEB.BAT", "WEB"),
    "TASKMGR": ("apps/taskmgr/TASKMGR.BAT", "TMGR"),
    "CONTROL": ("apps/controlflow/CONTROL.BAT", "CONTROL"),
    "WRITER": ("apps/writer/WRITER.BAT", "WRITER"),
    "HYBRID": ("apps/hybrid/HYBRID.BAT", "HYBRID"),
    "PKGTEST": ("apps/pkgtest/PKGTEST.BAT", "PKGTEST"),
    "UITEST": ("apps/uitest/UITEST.BAT", "UITEST"),
    "COMPANION": ("apps/companion/COMPANION.BAT", "COMPANION"),
    "SYS": ("apps/companion/SYS.BAT", "SYS"),
    "FILES": ("apps/companion/FILES.BAT", "FILES"),
    "NET": ("apps/companion/NET.BAT", "NET"),
    "FUN": ("apps/companion/FUN.BAT", "FUN"),
    "SET": ("apps/companion/SET.BAT", "SET"),
    "AGENDA": ("apps/companion/AGENDA.BAT", "AGENDA"),
    "SVC": ("apps/companion/SVC.BAT", "SVC"),
    "MEMLOG": ("apps/companion/MEMLOG.BAT", "MEMLOG"),
    "LIB": ("apps/companion/LIB.BAT", "LIB-COMPANION"),
    "SHOWCASE": ("apps/showcase/SHOWCASE.BAT", "SHOWCASE"),
}

# Host-unsupported surfaces, with the honest reason for the SKIP. A token is
# looked for in the REAL app source, never in the fixture.
HARDWARE: Tuple[Tuple[str, str], ...] = (
    ("gfx ", "gfx RGB565 canvas (lineaa/circleaa/gradient/composite/blitmany)"),
    ("draw run ", "TUI /fg|/bg colour-run surface"),
    ("draw sprite ", "TUI procedural sprite sampler"),
    ("tui frame ", "TUI frame/dirty-rect diagnostics"),
    ("tui focus ", "TUI focus ring"),
    ("tui layout ", "TUI layout geometry"),
    ("tui scroll ", "TUI named scroll regions"),
    ("tui route ", "TUI key routing"),
    ("mpick ", "modal scrollable picker"),
    ("mprog ", "modal async progress"),
    ("mformx ", "modal quick form"),
    ("mstack ", "modal suspended-session stack"),
    ("webapp ", "browser/JS webapp facade"),
    ("html ", "browser page surface"),
    ("natrun ", "signed native P4X loader"),
)

NEW_TOKENS = (
    "str /", "map /", "for /K", "func ", "trap /on",
    "gfx gradient", "gfx lineaa", "gfx circleaa", "gfx rectaa",
    "gfx composite", "gfx blitmany /a", "draw run ", "draw sprite ",
    "tui frame ", "tui focus ", "tui layout ", "tui scroll ", "tui route ",
    "mpick ", "mprog ", "mformx ", "mstack ",
    "webapp event", "webapp tick", "webapp js", "natrun ",
)

# The v1.7 DOS redesign publishes ONE structural line from each real app and
# its host-safe fixture. Parsing that line is an assertion on the app's own
# output (baselined by host_batch_run), not a second app model.
DOS_LINE_RE = re.compile(
    r'^\[M-DOS-([A-Z0-9_-]+)\]\s+screen=(\S+)\s+panels=(\d+)\s+'
    r'head="([^"]*)"\s+theme=(amber|green|ice|mono)\s+bars=(\d+)\s+'
    r'table=([YN])\s+spark=(\d+)\s+status=([YN])\s*$')
DOS_THEMES = ("amber", "green", "ice", "mono")


@dataclass(frozen=True)
class DosStructure:
    tag: str
    screen: str
    panels: int
    head: str
    theme: str
    bars: int
    table: str
    spark: int
    status: str

    @property
    def fingerprint(self) -> Tuple[str, ...]:
        """Every field that must distinguish one app's DOS page from another."""
        return (self.screen, self.head, str(self.panels), self.theme,
                str(self.bars), self.table, str(self.spark), self.status)


def parse_dos_structure(transcript: Sequence[str], name: str) -> DosStructure:
    """Assert one well-formed DOS structure line, then return its fields."""
    matches = [DOS_LINE_RE.match(line.strip()) for line in transcript]
    lines = [m for m in matches if m is not None]
    if len(lines) != 1:
        raise GateError("%s emitted %d DOS structure lines (expected exactly 1)"
                        % (name, len(lines)))
    m = lines[0]
    st = DosStructure(m.group(1), m.group(2), int(m.group(3)), m.group(4),
                      m.group(5), int(m.group(6)), m.group(7), int(m.group(8)),
                      m.group(9))
    if st.tag.upper() != name.upper() and st.tag not in ("LIB-COMPANION",):
        raise GateError("%s DOS tag %r does not name the app" % (name, st.tag))
    if st.theme not in DOS_THEMES:
        raise GateError("%s has an unknown DOS theme %r" % (name, st.theme))
    if st.screen.endswith("-HEADLESS"):
        if (st.panels or st.bars or st.spark or st.table != "N" or
                st.status != "N" or st.head != "NONE" or st.theme != "mono"):
            raise GateError("%s headless DOS line is not an honest empty page: %s"
                            % (name, st.fingerprint))
    else:
        if st.panels < 2:
            raise GateError("%s DOS page has %d panels (needs a framed layout)"
                            % (name, st.panels))
        if len(st.head) < 3 or st.head.upper() == "NONE":
            raise GateError("%s DOS page has no real header: %r" % (name, st.head))
        if st.theme == "mono":
            raise GateError("%s DOS page has no DOS colour theme" % name)
        if not (st.bars or st.spark or st.table == "Y" or st.status == "Y"):
            raise GateError("%s DOS page has no panel content at all" % name)
    return st


def register_structure(structures: Dict[str, str], name: str,
                       structure: DosStructure) -> None:
    """Two apps may not publish the same DOS fingerprint."""
    seen = structures.setdefault(structure.fingerprint, name)
    if seen != name:
        raise GateError("%s duplicate DOS structure of %s: %s"
                        % (name, seen, " ".join(structure.fingerprint)))


def executable_lines(source: str) -> str:
    """Comments are opaque in batch; never count a verb mentioned in `rem`."""
    keep = []
    for line in source.splitlines():
        stripped = line.strip()
        if stripped.startswith("::") or re.match(r"(?i)^@?rem(\s|$)", stripped):
            continue
        keep.append(line)
    return "\n".join(keep)


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


def load_batch_runner():
    """Import the existing gate as a library - no second interpreter/gate."""
    path = TOOLS / "host_batch_run.py"
    if not path.is_file():
        raise GateError("host batch gate library is missing: %s" % path)
    spec = importlib.util.spec_from_file_location("p4_host_batch_run", path)
    if spec is None or spec.loader is None:
        raise GateError("cannot load %s" % path)
    module = importlib.util.module_from_spec(spec)
    # dataclasses in the imported module need it registered before exec.
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as exc:  # pragma: no cover - surfaced honestly
        raise GateError("cannot import %s: %s" % (path, exc)) from exc
    return module


def check_source(source: str, name: str, tag: str) -> None:
    """Tie the host extract to a real app that carries the rich marker."""
    marker = "[M-RICH-%s]" % tag
    if marker not in source:
        raise GateError("%s source is missing %s" % (name, marker))
    body = executable_lines(source)
    used = [token for token in NEW_TOKENS if token in body]
    if not used:
        raise GateError("%s source has no additive surface in executable lines" % name)


def hardware_skips(source: str) -> List[Tuple[str, str]]:
    """List hardware surfaces found in the real app, each an honest SKIP."""
    found: List[Tuple[str, str]] = []
    body = executable_lines(source)
    for token, reason in HARDWARE:
        if token in body:
            found.append((token.strip(), reason))
    if not any(tok.startswith("draw ") for tok, _ in found):
        if re.search(r"(?im)^\s*draw\s", body):
            found.append(("draw", "TUI cell surface"))
    return found


def format_app(name: str, assertions: int, skips: List[Tuple[str, str]]) -> str:
    if skips:
        parts = ", ".join("%s (%s)" % (tok, reason) for tok, reason in skips)
        return "SKIP %-11s %3d host assertion(s); %d hardware surface(s) not host-tested: %s" % (
            name, assertions, len(skips), parts)
    return "PASS %-11s %3d host assertion(s); 0 hardware surfaces" % (name, assertions)


def run_gate(hbr, update: bool) -> int:
    if not FIXTURES.is_dir():
        raise GateError("fixture directory is missing: %s" % FIXTURES)
    runner = hbr.discover_elf(None)
    total_assertions = 0
    total_skips = 0
    structures: Dict[str, str] = {}
    failures: List[str] = []
    for name, (source_rel, tag) in APPS.items():
        fixture = FIXTURES / (name + ".BAT")
        if not fixture.is_file():
            raise GateError("fixture is missing: %s" % fixture)
        source_path = ROOT / source_rel
        if not source_path.is_file():
            raise GateError("app source is missing: %s" % source_path)
        source = source_path.read_text(encoding="utf-8", errors="replace")
        try:
            check_source(source, name, tag)
            raw = hbr.run_elf(runner, FIXTURES, name)
            _note, result = hbr.evaluate(raw, name, BASELINE, update)
        except (GateError, hbr.GateError) as exc:
            failures.append("FAIL %-11s %s" % (name, exc))
            continue
        skips = hardware_skips(source)
        try:
            structure = parse_dos_structure(result.transcript, name)
        except GateError as exc:
            failures.append("FAIL %-11s %s" % (name, exc))
            continue
        try:
            register_structure(structures, name, structure)
        except GateError as exc:
            failures.append("FAIL %-11s %s" % (name, exc))
            continue
        assertions = result.assertions + 8   # tag, fields, panel/header/theme/content, uniqueness
        total_assertions += assertions
        total_skips += len(skips)
        print("  " + format_app(name, assertions, skips))
        print("      DOS %s panels=%d head=%r theme=%s bars=%d table=%s "
              "spark=%d status=%s" % (structure.screen, structure.panels,
                                      structure.head, structure.theme,
                                      structure.bars, structure.table,
                                      structure.spark, structure.status))
    if failures:
        for line in failures:
            print("  " + line, file=sys.stderr)
        print("apps gate %s: FAIL (%d/%d apps failed)" % (
            "UPDATE" if update else "CHECK", len(failures), len(APPS)), file=sys.stderr)
        return 1
    print("apps gate %s: %d apps, %d host assertion(s) PASS, "
          "%d hardware surface(s) SKIP (honest, not faked)" % (
              "UPDATE" if update else "CHECK", len(APPS), total_assertions, total_skips))
    return 0


def self_test(hbr) -> int:
    """Red canaries: a lax gate would be a defect, not a pass."""
    good = "\n".join([
        hbr.BEGIN,
        "[M-RICH-X] ok",
        hbr.DONE_MARKER,
        hbr.DONE,
        "ERRORLEVEL 0",
    ]) + "\n"
    checks: List[Tuple[str, bool]] = []
    import tempfile

    with tempfile.TemporaryDirectory(prefix="apps_gate_selftest_") as tmp:
        base = Path(tmp) / "baseline"
        result = hbr.parse_emission(good, "X")
        hbr.write_baseline(base / "X.txt", "X", result)
        try:
            hbr.evaluate(good, "X", base, update=False)
            checks.append(("clean host report accepted", True))
        except hbr.GateError:
            checks.append(("clean host report accepted", False))

        try:
            hbr.evaluate(good.replace("[M-RICH-X] ok", "[M-RICH-X] BAD"),
                         "X", base, update=False)
            checks.append(("changed baseline rejected", False))
        except hbr.GateError:
            checks.append(("changed baseline rejected", True))

        try:
            hbr.evaluate(good.replace(hbr.DONE_MARKER + "\n", ""),
                         "X", base, update=False)
            checks.append(("missing done marker rejected", False))
        except hbr.GateError:
            checks.append(("missing done marker rejected", True))

        stale = base / "X.txt"
        stale.write_text(stale.read_text(encoding="utf-8").replace(
            hbr.BASELINE_TAG, "batch-baseline-v0"), encoding="utf-8")
        try:
            hbr.evaluate(good, "X", base, update=False)
            checks.append(("stale baseline tag rejected", False))
        except hbr.GateError:
            checks.append(("stale baseline tag rejected", True))

        try:
            hbr.parse_emission(good.replace("[M-RICH-X] ok",
                                            "batch: unsupported command on this host lane: gfx"),
                               "X")
            checks.append(("unsupported verb rejected", False))
        except hbr.GateError:
            checks.append(("unsupported verb rejected", True))

        try:
            check_source("[M-RICH-X] only", "X", "X")
            checks.append(("source without new verbs rejected", False))
        except GateError:
            checks.append(("source without new verbs rejected", True))

        try:
            check_source("str /rev A B\nmap /set M k v", "X", "OTHER")
            checks.append(("source without rich marker rejected", False))
        except GateError:
            checks.append(("source without rich marker rejected", True))

    # DOS-structure canaries: a fixture with no distinct dashboard identity
    # must never satisfy this gate, even when its baseline matches exactly.
    dos_good = '[M-DOS-X] screen=X-OVERVIEW panels=3 head="EXPLORER" theme=amber bars=2 table=Y spark=1 status=Y'
    try:
        parse_dos_structure([dos_good], "X")
        checks.append(("clean DOS structure accepted", True))
    except GateError:
        checks.append(("clean DOS structure accepted", False))
    for label, lines in (
        ("missing DOS structure rejected", []),
        ("two DOS structures rejected", [dos_good, dos_good]),
        ("fake headless DOS page rejected",
         ['[M-DOS-X] screen=X-HEADLESS panels=3 head="NONE" theme=mono '
          'bars=0 table=N spark=0 status=N']),
        ("unframed DOS page rejected",
         ['[M-DOS-X] screen=X-OVERVIEW panels=1 head="EXPLORER" theme=amber '
          'bars=0 table=N spark=0 status=Y']),
    ):
        try:
            parse_dos_structure(lines, "X")
            checks.append((label, False))
        except GateError:
            checks.append((label, True))
    try:
        seen: Dict[str, str] = {}
        register_structure(seen, "X", parse_dos_structure([dos_good], "X"))
        register_structure(seen, "Y", parse_dos_structure(
            [dos_good.replace("[M-DOS-X]", "[M-DOS-Y]")], "Y"))
        checks.append(("duplicate DOS structure rejected", False))
    except GateError:
        checks.append(("duplicate DOS structure rejected", True))

    for label, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", label))
    ok = bool(checks) and all(item[1] for item in checks)
    print("apps gate self-test: %s (%d canaries)" % ("PASS" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="compare all app fixtures against baselines (default)")
    mode.add_argument("--update", action="store_true",
                      help="rewrite all app baselines from the real ELF output")
    parser.add_argument("--self-test", action="store_true",
                        help="red canaries for the gate itself (no ELF needed)")
    args = parser.parse_args(argv)
    try:
        hbr = load_batch_runner()
        if args.self_test:
            return self_test(hbr)
        return run_gate(hbr, args.update)
    except GateError as exc:
        print("apps gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
