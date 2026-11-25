#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_all_gates.py - ONE merged runner for every board-free gate.

This tool is a *driver*, not another implementation of a test. Every row below
invokes the shipped gate script (or, for the Unity manifest static check, its
own module) with the same arguments the individual gate documents. It adds:
  * one PASS/FAIL table with counts and a non-zero exit on any failure;
  * ``--list`` / ``--only`` so a subset can be run without editing code;
  * shared POSIX-ELF discovery and WSL routing for the one host gate that
    executes an ELF directly (``host_unit_run.py --static``). The other gates
    already route their own ELFs through WSL; no path is hardcoded here.

A pass is a BOARD-FREE PRE-GATE ONLY. It says nothing about on-target behaviour
and is never hardware sign-off: ``harness.md`` §0 still governs sign-off.

Usage:
    python tools/host_all_gates.py                 # every gate
    python tools/host_all_gates.py --list          # gate ids and what they run
    python tools/host_all_gates.py --only strict,tui,modal
    python tools/host_all_gates.py --only selftests   # every red canary
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
PY = sys.executable
RUN_TIMEOUT = 300


# ---------------------------------------------------------------------------
# Shared POSIX-ELF discovery / WSL routing (no hardcoded drive or build path).
# ---------------------------------------------------------------------------
def discover_elf(name: str) -> Optional[Path]:
    """Find one host ELF under the repo's build dirs, preferring build-wsl."""
    found = set()
    for pattern in ("test/host/build*/" + name,
                    "tools/host/build*/" + name,
                    "build*/" + name):
        found.update(p for p in ROOT.glob(pattern) if p.is_file())
    if not found:
        return None

    def preference(path: Path) -> Tuple[int, str]:
        parts = {part.lower() for part in path.parts}
        if "build-wsl" in parts:
            return (0, str(path))
        if "build-win" in parts:
            return (1, str(path))
        return (2, str(path))

    return min(found, key=preference).resolve()


def is_posix_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def to_wsl_path(path: Path) -> str:
    """Map a Windows drive path onto its WSL mount (no hardcoded drive letter)."""
    text = str(path.resolve()).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/(.*)$", text)
    if match:
        return "/mnt/%s/%s" % (match.group(1).lower(), match.group(2))
    return text


def run_command(argv: Sequence[str], *, cwd: Optional[Path] = None,
                timeout: int = RUN_TIMEOUT) -> subprocess.CompletedProcess:
    """Run a native command, routing a POSIX ELF through WSL on Windows."""
    exe = Path(argv[0])
    if os.name == "nt" and exe.is_file() and is_posix_elf(exe):
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if wsl is None:
            raise RuntimeError("POSIX ELF %s needs WSL, but wsl.exe is absent" % exe)
        argv = [wsl, "--exec", to_wsl_path(exe)] + [to_wsl_path(Path(a)) if
                Path(a).exists() else a for a in argv[1:]]
        cwd = None
    return subprocess.run(list(argv), cwd=str(cwd) if cwd else None,
                          capture_output=True, text=True, timeout=timeout)


def load_module(name: str, path: Path):
    """Import a shipped gate as a library (dataclasses need sys.modules)."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def unity_static() -> Tuple[int, str]:
    """Run host_unit_run.py --static with a WSL-routed runner for its ELF.

    host_unit_run.py executes ``test/host/build/.../*.elf`` directly; on Windows
    that is a POSIX binary. Rather than fork a second manifest checker, reuse
    the shipped module and only replace its ELF launcher with the shared
    routing helper above.
    """
    module = load_module("p4_all_gates_host_unit_run", TOOLS / "host_unit_run.py")
    elf = discover_elf("p4minishell_host_tests.elf")
    if elf is not None:
        module.ELF = str(elf)

        def routed(verbose: bool = False):
            try:
                proc = run_command([str(elf)], timeout=180)
            except (OSError, subprocess.TimeoutExpired, RuntimeError):
                return None
            text = module.ANSI.sub("", (proc.stdout + proc.stderr).decode(
                "utf-8", errors="replace") if isinstance(proc.stdout, bytes)
                else proc.stdout + proc.stderr)
            if verbose:
                print(text)
            return proc.returncode, text

        module.run_elf = routed
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = module.static_checks()
    return rc, buf.getvalue()


# ---------------------------------------------------------------------------
# Gate registry. One row per shipped capability; self-tests are first-class.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Gate:
    gid: str
    title: str
    argv: Tuple[str, ...] = field(default_factory=tuple)
    group: str = "gate"
    note: str = ""


GATES: Tuple[Gate, ...] = (
    # Static structure / false-green guards.
    Gate("strict", "green_guard --strict", ("green_guard.py", "--strict"), "static",
         "vacuous Unity/host suites and declared-but-unrun tests"),
    Gate("guard_canary", "green_guard self-test", ("green_guard.py", "--self-test"),
         "selftest", "red canaries for each false-green guard"),
    Gate("comments", "comment tags", ("comment_index.py",), "static"),
    Gate("breadcrumbs", "breadcrumb map", ("breadcrumbs.py",), "static"),
    Gate("verify", "harness/verify", ("harness", "verify.py"), "static",
         "aggregate static gates + harness marker self-check"),
    Gate("gallery", "gallery identity", ("gallery_audit.py",), "static"),
    Gate("gallery_canary", "gallery identity canary",
         ("gallery_audit_selftest.py",), "selftest"),

    # Host unit lane.
    Gate("gen", "host runner manifest", ("host_gen_runner.py", "--check"), "unity"),
    Gate("unity_static", "Unity static (host lane)", (), "unity",
         "manifest drift + case count via the shipped host_unit_run.py"),

    # Real-code host gates.
    Gate("batch", "batch interpreter fixtures", ("host_batch_run.py", "--check"), "host"),
    Gate("batch_canary", "batch fixture canary",
         ("host_batch_run.py", "--self-test"), "selftest"),
    Gate("webapp", "webapp render fixtures", ("host_webapp_gate.py", "--check"), "host"),
    Gate("webapp_canary", "webapp render canary",
         ("host_webapp_gate.py", "--self-test"), "selftest"),
    # REAL-EXECUTION browser contract: drives the shipped host_webapp_render.elf
    # on wiki_esp32 + four showcases, asserts emitted H1/anchor/spec/image lines,
    # exact baselines and STATIC link/keyboard wiring. No board, no fake render.
    Gate("browser", "browser real-render contract",
         ("host_browser_gate.py", "--check"), "host",
         "real ELF: wiki H1/fg=11 anchor/240 MHz/infobox/image outcomes, "
         "four DOS showcases, exact baselines, static wiring only"),
    Gate("browser_canary", "browser contract canary",
         ("host_browser_gate.py", "--self-test"), "selftest",
         "wrong page, missing infobox, uncoloured link, dropped image, "
         "tofu, blank render, broken keyboard/link wiring"),
    Gate("tui", "TUI cell-frame scenes", ("host_tui_gate.py", "--check"), "host"),
    Gate("tui_canary", "TUI scene canary",
         ("host_tui_gate.py", "--self-test"), "selftest"),
    Gate("modal", "modal state machine", ("host_modal_gate.py", "--check"), "host"),
    Gate("modal_canary", "modal state canary",
         ("host_modal_gate.py", "--self-test"), "selftest"),
    Gate("gfx", "gfx toolkit wiring", ("gfx_toolkit_test.py", "--host"), "host"),
    Gate("gfx_canary", "gfx toolkit canary",
         ("gfx_toolkit_test.py", "--self-test"), "selftest"),
    Gate("apps", "reference apps fixtures", ("host_apps_gate.py", "--check"), "host"),
    Gate("apps_canary", "reference apps canary",
         ("host_apps_gate.py", "--self-test"), "selftest"),

    # Deterministic host proofs and the native payload checker.
    Gate("gfx_proof", "gfx proof image", ("make_gfx_proof.py", "--check"), "proof"),
    Gate("gfx_proof_canary", "gfx proof canary",
         ("make_gfx_proof.py", "--self-test"), "selftest"),
    Gate("web_proof", "webapp proof images", ("make_webapp_proof.py", "--check"), "proof"),
    Gate("web_proof_canary", "webapp proof canary",
         ("make_webapp_proof.py", "--self-test"), "selftest"),
    Gate("visual", "DOS visual proof quality", ("visual_proof_gate.py", "--check"), "proof",
         "1024x600, DOS palette, CP437 box frames, ink bands, no blue/tofu, "
         "distinct signatures - host proofs only, never hardware sign-off"),
    Gate("visual_canary", "visual proof canary",
         ("visual_proof_gate.py", "--self-test"), "selftest"),
    # Deterministic proof images driven by the shipped real-code host gate.
    # These are board-free pre-gates only, never hardware sign-off.
    Gate("web_screens", "web screenshot proofs",
         ("make_web_screenshots.py", "--check"), "proof",
         "real HTML in the one DOS renderer: cell grid, CP437 frame, "
         "palette, feature report, distinct signatures"),
    Gate("web_screens_canary", "web screenshot proof canary",
         ("make_web_screenshots.py", "--self-test"), "selftest"),
    Gate("apps_proof", "app dashboard proofs",
         ("make_app_screenshots.py", "--check"), "proof",
         "gated APPLIB cell grids: frame, palette, identity, no shell chrome"),
    Gate("apps_proof_canary", "app dashboard proof canary",
         ("make_app_screenshots.py", "--self-test"), "selftest"),
    Gate("native_canary", "native payload tamper canary",
         ("make_native_payload.py", "selftest"), "selftest"),
    Gate("native_verify", "signed demo payload verify",
         ("make_native_payload.py", "verify", "--in",
          "tools/host/fixtures/native/demo.P4X", "--pub",
          "tools/host/fixtures/native/testkey.pub.bin"), "proof"),
)
GATE_BY_ID: Dict[str, Gate] = {gate.gid: gate for gate in GATES}
SELFTEST_IDS = tuple(g.gid for g in GATES if g.group == "selftest")


def run_gate(gate: Gate) -> Tuple[int, str]:
    """Execute one gate; return (returncode, captured output)."""
    if gate.gid == "unity_static":
        return unity_static()
    parts = list(gate.argv)
    if parts and parts[0] == "harness":
        script = TOOLS / "harness" / parts[1]
        parts = parts[2:]
    else:
        script = TOOLS / parts[0]
        parts = parts[1:]
    if not script.is_file():
        return 1, "missing gate script: %s\n" % script
    args = [PY, str(script), *parts]
    try:
        proc = run_command(args, cwd=ROOT)
    except subprocess.TimeoutExpired:
        return 1, "gate timed out after %ds: %s\n" % (RUN_TIMEOUT, gate.gid)
    except (OSError, RuntimeError) as exc:
        return 1, "cannot run %s: %s\n" % (gate.gid, exc)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def tidy(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n").strip("\n")


def select_gates(args) -> List[Gate]:
    if not args.only:
        return list(GATES)
    ids = []
    for token in args.only:
        ids.extend(part.strip() for part in token.split(",") if part.strip())
    unknown = [gid for gid in ids if gid not in GATE_BY_ID]
    if "selftests" in ids:
        ids = [gid for gid in ids if gid != "selftests"] + list(SELFTEST_IDS)
        unknown = [gid for gid in ids if gid not in GATE_BY_ID]
    if unknown:
        raise ValueError("unknown gate id(s): %s (use --list)" % ", ".join(unknown))
    seen = set()
    return [GATE_BY_ID[gid] for gid in ids if not (gid in seen or seen.add(gid))]


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="list gate ids and exit")
    parser.add_argument("--only", nargs="+", metavar="ID",
                        help="run only these gate ids (comma or space separated; "
                             "'selftests' selects every canary)")
    parser.add_argument("--verbose", action="store_true",
                        help="echo each gate's full output")
    args = parser.parse_args(argv)

    if args.list:
        print("%-20s %-10s %s" % ("ID", "GROUP", "WHAT IT RUNS"))
        for gate in GATES:
            cmd = " ".join(gate.argv) if gate.argv else "host_unit_run.py --static"
            print("%-20s %-10s %s" % (gate.gid, gate.group, cmd))
            if gate.note:
                print("%-20s %s" % ("", gate.note))
        print("\n%d gates (%d canaries)" % (len(GATES), len(SELFTEST_IDS)))
        return 0

    try:
        gates = select_gates(args)
    except ValueError as exc:
        print("host_all_gates: %s" % exc, file=sys.stderr)
        return 2

    print("HOST-ONLY PRE-GATES - NO BOARD - NOT HARDWARE SIGN-OFF")
    print("harness.md section 0 still governs hardware sign-off.\n")
    results: List[Tuple[Gate, int, float, str]] = []
    for gate in gates:
        started = time.time()
        rc, output = run_gate(gate)
        duration = time.time() - started
        results.append((gate, rc, duration, output))
        print("  [%s] %-20s %5.1fs  %s" % ("PASS" if rc == 0 else "FAIL",
                                           gate.gid, duration, gate.title))
        if rc == 0 and args.verbose:
            for line in tidy(output).splitlines():
                print("      " + line)
        if rc != 0:
            lines = tidy(output).splitlines()
            for line in lines[-18:]:
                print("      " + line, file=sys.stderr)

    passed = sum(1 for _g, rc, _d, _o in results if rc == 0)
    failed = len(results) - passed
    print("\n%-20s %-6s %s" % ("GATE", "RESULT", "DETAIL"))
    for gate, rc, _duration, _output in results:
        detail = gate.note or (" ".join(gate.argv) if gate.argv else
                               "host_unit_run.py --static")
        print("%-20s %-6s %s" % (gate.gid, "PASS" if rc == 0 else "FAIL", detail))
    print("\nHOST ALL GATES: %d pass, %d fail, %d total (%d canaries)"
          % (passed, failed, len(results),
             sum(1 for g, _r, _d, _o in results if g.group == "selftest")))
    print("VERDICT: %s (board-free pre-gates only; NOT hardware sign-off)"
          % ("PASS" if failed == 0 else "FAIL"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
