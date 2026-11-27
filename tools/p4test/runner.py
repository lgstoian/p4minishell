# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Suite discovery and orchestration.

A suite is a module under ``tools/suites/`` exposing:

* ``NAME``      - human name shown in the report.
* ``TAGS``      - optional list (``"sd"``, ``"network"``, ``"slow"``, ...).
* ``run(dev, ctx)`` - returns a :class:`~p4test.asserts.Checklist` or
  :class:`~p4test.asserts.SuiteResult`.

The runner opens ONE device session per board, runs the selected suites in
order, and prints a single PASS/FAIL/SKIP table with a non-zero exit on
failure. A suite that reports only skipped checks (a missing prerequisite) is
SKIP: counted separately, never a ``PASS 0/1`` and never a failure. A suite
that asserts nothing stays a failure (no false green).
With ``--boards`` several boards run concurrently (one thread each) and the
report merges per-board results, flags board-only failures, and (under
fail-fast) stops the other boards at the first failure.
"""
from __future__ import annotations

import sys
try:
    # Suites print rendered page dumps; a cell grid can contain block
    # glyphs and the Windows console is cp1252. Never let console
    # encoding turn a test result into a crash.
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
except Exception:
    pass

import argparse
import importlib
import io
import json
import os
import pkgutil
import re
import subprocess
import sys
import threading
import time
import traceback
from typing import Dict, List, Optional, Tuple

from .agent import dogfood_self_test
from .asserts import Checklist, SuiteResult
from .device import Device
from .local_config import wifi_credentials
from .session import PANICS, P4Error, PanicError, read_until_self_test
from . import panic_symbols

# Fast tier: the per-area smoke subset that gates feature delivery in
# minutes. Full tier (default) is every discovered suite and gates releases.
TIER_FAST = [
    "s01_smoke",
    "s05_storage",
    "s06_batch",
    "s12_power_audio",
    "s11_connectivity",
]

_print_lock = threading.Lock()


class _ThreadRouter:
    """Process-wide stdout/stderr proxy that routes by thread.

    ``contextlib.redirect_stdout`` is process-global and corrupts output when
    two board threads nest it (each exit restores a stale target, swallowing
    later prints). This router is installed once for the whole multi-board
    run instead: a thread with a buffer set captures into it, every other
    thread (and the main thread) writes straight through.
    """

    def __init__(self) -> None:
        self._local = threading.local()
        self._real_out = sys.stdout
        self._real_err = sys.stderr
        self._lock = threading.Lock()

    def _buf(self):
        return getattr(self._local, "buf", None)

    def set_buffer(self, buf) -> None:
        self._local.buf = buf

    def clear_buffer(self) -> None:
        self._local.buf = None

    def write(self, s) -> int:
        buf = self._buf()
        if buf is None:
            with self._lock:
                self._real_out.write(s)
            return len(s)
        return buf.write(s)

    def write_err(self, s) -> int:
        buf = self._buf()
        if buf is None:
            with self._lock:
                self._real_err.write(s)
            return len(s)
        return buf.write(s)

    def flush(self) -> None:
        buf = self._buf()
        if buf is None:
            with self._lock:
                self._real_out.flush()
        else:
            buf.flush()

    @property
    def encoding(self):
        return getattr(self._real_out, "encoding", "utf-8")

    @property
    def errors(self):
        return getattr(self._real_out, "errors", "replace")


class _StdErrProxy:
    """Routes sys.stderr writes through the shared router."""

    def __init__(self, router: _ThreadRouter) -> None:
        self._router = router

    def write(self, s) -> int:
        return self._router.write_err(s)

    def flush(self) -> None:
        self._router.flush()

    @property
    def encoding(self):
        return self._router.encoding

    @property
    def errors(self):
        return self._router.errors


def discover(suites_pkg: str = "suites") -> List:
    """Import every suite module in order and return them sorted by name."""
    sys.path.insert(0, _tools_dir())
    pkg = importlib.import_module(suites_pkg)
    mods = []
    for info in sorted(pkgutil.iter_modules(pkg.__path__), key=lambda i: i.name):
        if info.name.startswith("_"):
            continue
        mod = importlib.import_module("%s.%s" % (suites_pkg, info.name))
        if hasattr(mod, "run") and hasattr(mod, "NAME"):
            mods.append(mod)
    return mods


def _tools_dir() -> str:
    import os
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Static gates that run before (or instead of) hardware. They are the host-side
# false-green / machine-readable-comment checks: a green hardware run is only
# meaningful if the suite actually asserts and the tree is structurally sound.
STATIC_GATES = [
    ("comment index", "comment_index.py"),
    ("breadcrumbs", "breadcrumbs.py"),
    ("green guard (false-green tests)", "green_guard.py", ["--strict"]),
    ("ui colours", "check_assets.py"),
    # Artifact identity: reject a gallery PNG that shows the shell or is the
    # same screen as another app. This is the gate that v1.4.3 lacked.
    ("gallery identity", "gallery_audit.py"),
    ("gallery guard canary", "gallery_audit_selftest.py"),
    ("LED state-machine contract", "led_contract.py"),
]
# The static-DRAM budget needs a built image; a fresh checkout has none, so
# this gate is conditional exactly as in tools/harness/verify.py.
SIZE_GATE = ("size budget", "size_budget.py")


def elf_exists() -> bool:
    return os.path.exists(os.path.join(os.path.dirname(_tools_dir()),
                                       "build", "p4minishell.elf"))


def static_gate_specs(include_size: Optional[bool] = None) -> list:
    """The host gate list shared by ``--static``/``--guard`` and verify.py."""
    specs = [tuple(spec) for spec in STATIC_GATES]
    want_size = elf_exists() if include_size is None else include_size
    if want_size:
        specs.append(SIZE_GATE)
    return specs


def run_static_gates() -> int:
    """Run the host-only gates; return non-zero if any fails."""
    tools = _tools_dir()
    root = os.path.dirname(tools)
    failed = 0
    for spec in static_gate_specs():
        label, script = spec[:2]
        args = spec[2] if len(spec) > 2 else ()
        print("\n" + "=" * 62)
        print("STATIC GATE: %s" % label)
        print("=" * 62)
        rc = subprocess.run([sys.executable, os.path.join(tools, script)] + list(args),
                            cwd=root).returncode
        if rc != 0:
            failed += 1
            print("STATIC GATE FAILED: %s" % label)
    print("\nSTATIC GATES %s" % ("OK" if failed == 0 else "FAIL"))
    return 1 if failed else 0


def needs_clean_boot(mod) -> bool:
    """True when a suite declares that prior UI/pixel state contaminates it."""
    return bool(getattr(mod, "NEEDS_CLEAN_BOOT", False))


def suite_wedged(res: SuiteResult) -> bool:
    """True when a failed suite may have left the shell worker blocked."""
    err = res.error or ""
    return ("not seen within" in err or "did not return" in err
            or "marker not seen" in err)


def _run_one_suite(mod, dev, ctx) -> Tuple[SuiteResult, List[str]]:
    """Run a single suite, returning its result plus failed check names."""
    name = getattr(mod, "NAME", mod.__name__)
    started = time.time()
    try:
        out = mod.run(dev, ctx)
        if isinstance(out, Checklist):
            out.print_report()
            res = out.result()
            failed = [c.name for c in out.checks if not c.ok]
            return res, failed
        if isinstance(out, SuiteResult):
            return out, ([out.error] if out.error else [])
        return (SuiteResult(name, 0, 1,
                            error="suite returned %r" % type(out)),
                ["bad return %r" % type(out)])
    except PanicError as exc:
        # Attach symbols from the matching ELF: a raw backtrace otherwise
        # costs a manual addr2line round before triage can start.
        symbols = panic_symbols.symbolize(
            str(exc), getattr(getattr(dev, "session", None), "port", None))
        detail = "PANIC: %s%s" % (exc, ("\n" + symbols) if symbols else "")
        print("  [PANIC] %s%s" % (exc, ("\n" + symbols) if symbols else ""))
        return (SuiteResult(name, 0, 1, duration=time.time() - started,
                            error=detail),
                ["panic"])
    except P4Error as exc:
        print("  [ERROR] %s" % exc)
        return (SuiteResult(name, 0, 1, duration=time.time() - started,
                            error=str(exc)),
                ["p4error"])
    except Exception as exc:  # noqa: BLE001
        traceback.print_exc()
        return (SuiteResult(name, 0, 1, duration=time.time() - started,
                            error="%s: %s" % (type(exc).__name__, exc)),
                ["exception"])


def _is_skip(res: SuiteResult) -> bool:
    """True when a suite recorded only skipped checks (an honest SKIP).

    A suite that needs a prerequisite the board lacks (an app on a USB stick,
    a sensor) returns ``SuiteResult(skipped=1)``: it asserted nothing because
    there was nothing to assert, and reporting it as ``PASS 0/1`` would read
    like verified coverage. Zero-assertion non-skipped suites stay failures:
    they have ``skipped == 0`` and ``SuiteResult.ok`` is already False for
    them (``vacuous``), so they can never be mistaken for a skip here.
    """
    return res.error is None and res.failed == 0 and res.skipped > 0


def format_suite_summary(results) -> Tuple[List[str], int, int]:
    """Per-suite table rows plus (failed, skipped) counts - no device needed.

    Pure so the SKIP/PASS/FAIL/zero-assertion verdict can be checked on the
    host (``p4test_run.py --self-test``) without a board. A suite that only
    skipped checks is SKIP; a suite that asserted nothing is still FAIL.
    """
    rows: List[str] = []
    failed = 0
    skipped = 0
    for r in results:
        extra = (" - %s" % r.error) if r.error else ""
        if _is_skip(r):
            skipped += 1
            rows.append("  %-4s %-34s %3d skipped%s"
                        % ("SKIP", r.name[:34], r.skipped, extra))
            continue
        if not r.ok:
            failed += 1
        rows.append("  %-4s %-34s %3d/%3d checks%s"
                    % ("PASS" if r.ok else "FAIL", r.name[:34],
                       r.passed, r.total, extra))
    return rows, failed, skipped


def run_suites(mods, port: Optional[str], quick: bool = False,
               keep_going: bool = True) -> int:
    results: List[SuiteResult] = []
    dev = Device(port, boot=True, settle_ms=4000 if quick else 8000)
    ctx = {"quick": quick, "wifi": wifi_credentials()}
    settle_ms = 4000 if quick else 8000
    dirty = False
    try:
        for index, mod in enumerate(mods):
            name = getattr(mod, "NAME", mod.__name__)
            print("\n" + "=" * 62)
            print("SUITE: %s" % name)
            print("=" * 62)
            # A suite that declares NEEDS_CLEAN_BOOT must not inherit a TUI
            # page, rotation, sprite or modal from the one before it. A suite
            # that wedged the worker likewise leaves the board unusable. Other
            # suites share the boot: a hard reset per area cost ~8 min/board
            # and caught nothing the declared flag does not already cover.
            if index and (dirty or needs_clean_boot(mod)):
                try:
                    print("  [REBOOT] before %s (%s)" % (
                        name, "declared NEEDS_CLEAN_BOOT" if needs_clean_boot(mod)
                        else "previous suite wedged the shell"))
                    dev.reboot(settle_ms=settle_ms)
                except PanicError:
                    raise
                except Exception as exc:  # noqa: BLE001
                    res = SuiteResult(name, 0, 1,
                                      error="clean boot failed: %s" % exc)
                    results.append(res)
                    dirty = True
                    if not keep_going:
                        break
                    continue
                dirty = False
            res, _failed = _run_one_suite(mod, dev, ctx)
            results.append(res)
            if not res.ok and suite_wedged(res):
                dirty = True
            if not keep_going and results and not results[-1].ok:
                break
    finally:
        dev.close()

    print("\n" + "=" * 62)
    print("REGRESSION SUMMARY")
    print("=" * 62)
    rows, failed, skipped = format_suite_summary(results)
    for row in rows:
        print(row)
    total = len(results)
    skip_note = (", %d skipped" % skipped) if skipped else ""
    print("\nRESULT %s (%d/%d suites%s)"
          % ("OK" if failed == 0 else "FAIL", total - failed - skipped, total,
             skip_note))
    return 1 if failed else 0


def _resolve_board_port(token: str) -> Tuple[str, str]:
    """Resolve a CLI board token (COM port or board slug) to (label, port).

    COMx tokens pass through; anything else goes through board_ports so
    `--boards m5stack_tab5` works without knowing COM numbers.
    """
    if token.upper().startswith("COM"):
        return token.upper(), token.upper()
    try:
        sys.path.insert(0, _tools_dir())
        import board_ports
        port = board_ports.resolve_port(token)
        if port:
            return token, port.upper()
    except Exception:
        pass
    return token, token.upper()


def _board_worker(label: str, port: str, mods, quick: bool,
                  stop_event: threading.Event, fail_fast: bool,
                  retry: int, strict_green: bool, out: list,
                  router: _ThreadRouter) -> None:
    """Run every suite on one board, capturing all prints for merged output."""
    buf = io.StringIO()
    board_results: List[Tuple[str, SuiteResult, List[str], bool]] = []
    flaky: List[str] = []
    setup_failed: Optional[str] = None
    router.set_buffer(buf)
    try:
        print("=" * 62)
        print("BOARD %s (%s)" % (label, port))
        print("=" * 62)
        dev = None
        try:
            dev = Device(port, boot=True,
                         settle_ms=4000 if quick else 8000)
            ctx = {"quick": quick, "wifi": wifi_credentials()}
            settle_ms = 4000 if quick else 8000
            dirty = False
            for index, mod in enumerate(mods):
                name = getattr(mod, "NAME", mod.__name__)
                if stop_event.is_set():
                    print("\nSUITE [%s]: %s SKIPPED (fail-fast)"
                          % (label, name))
                    board_results.append(
                        (name, SuiteResult(name, 0, 0, skipped=1), [], True))
                    continue
                print("\nSUITE [%s]: %s" % (label, name))
                # Honour the suite's declared NEEDS_CLEAN_BOOT: pixel/rotation
                # state from the previous suite changes those captures. A suite
                # that wedged the shell worker must also be followed by a reset
                # or its failures cascade into unrelated areas. Other suites
                # share the boot; a hard reset for every one cost ~8 min/board
                # and caught nothing the declaration does not already cover.
                if index and (dirty or needs_clean_boot(mod)):
                    try:
                        print("  [REBOOT] before %s (%s)" % (
                            name, "declared NEEDS_CLEAN_BOOT"
                            if needs_clean_boot(mod)
                            else "previous suite wedged the shell"))
                        dev.reboot(settle_ms=settle_ms)
                    except PanicError:
                        raise
                    except Exception as exc:  # noqa: BLE001
                        res = SuiteResult(name, 0, 1,
                                          error="clean boot failed: %s" % exc)
                        board_results.append((name, res, [name], False))
                        dirty = True
                        if fail_fast:
                            stop_event.set()
                        continue
                    dirty = False
                attempts = 1 + max(0, retry)
                res, failed = _run_one_suite(mod, dev, ctx)
                tried = 1
                while not res.ok and tried < attempts and not (
                        res.error or "").startswith("PANIC"):
                    tried += 1
                    print("  [RETRY %d/%d] %s" % (tried, attempts, name))
                    # A timed-out suite can leave the console reader suspended
                    # (an SDFX/BMPX transfer that was interrupted); retrying on
                    # that boot only produces marker timeouts in unrelated
                    # checks. Start the retry from a clean boot instead.
                    try:
                        dev.reboot(settle_ms=settle_ms)
                    except Exception as exc:  # noqa: BLE001
                        res = SuiteResult(name, 0, 1,
                                          error="retry reboot failed: %s" % exc)
                        failed = [name]
                        break
                    res, failed = _run_one_suite(mod, dev, ctx)
                if tried > 1 and res.ok:
                    flaky.append(name)
                    print("  [FLAKY] %s passed on attempt %d" % (name, tried))
                    if strict_green:
                        # Passing on retry is not a true green. Report it as a
                        # failure so a release gate cannot hide an intermittent
                        # defect behind a second attempt.
                        res = SuiteResult(name, 0, 1,
                                          error="passed only on retry "
                                                "(strict green)",
                                          duration=res.duration)
                        failed = [name]
                board_results.append((name, res, failed, False))
                if not res.ok and suite_wedged(res):
                    # Remember it: the next suite gets a reset before it runs,
                    # so a timeout cannot cascade into unrelated areas.
                    print("  [RESET] %s timed out; next suite will reboot" % name)
                    dirty = True
                if fail_fast and not res.ok:
                    stop_event.set()
        finally:
            if dev is not None:
                dev.close()
    except Exception as exc:  # noqa: BLE001
        setup_failed = "%s: %s" % (type(exc).__name__, exc)
        buf.write("\n[BOARD %s SETUP FAILED: %s]\n" % (label, setup_failed))
        board_results.append((label, SuiteResult(label, 0, 1,
                                                error="board setup failed"),
                              ["setup"], False))
        if fail_fast:
            stop_event.set()
    finally:
        router.clear_buffer()
    with _print_lock:
        print(buf.getvalue(), end="")
    out.append((label, port, board_results, flaky, setup_failed))


def run_boards(board_tokens: List[str], mods, quick: bool = False,
               fail_fast: bool = True, retry: int = 0, strict_green: bool = False,
               baseline: Optional[Dict[str, Dict[str, float]]] = None
               ) -> Tuple[int, Dict]:
    """Run the suites concurrently on every board; merged table + differentials.

    Returns (exit_code, report) where report maps board label to its results.
    ``baseline`` optionally maps board label to {suite: seconds} for
    slow-check warnings.
    """
    resolved = [_resolve_board_port(t) for t in board_tokens]
    stop_event = threading.Event()
    collected: list = []
    router = _ThreadRouter()
    old_out, old_err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = router, _StdErrProxy(router)
    try:
        threads = []
        for label, port in resolved:
            th = threading.Thread(target=_board_worker,
                                  args=(label, port, mods, quick, stop_event,
                                        fail_fast, retry, strict_green,
                                        collected, router),
                                  name="board-%s" % label, daemon=True)
            threads.append(th)
        for th in threads:
            th.start()
        for th in threads:
            th.join()
    finally:
        sys.stdout, sys.stderr = old_out, old_err

    by_board: Dict[str, list] = {}
    setup_failures: List[Tuple[str, str]] = []
    for label, port, results, flaky, setup_failed in collected:
        by_board[label] = (port, results, flaky)
        if setup_failed:
            setup_failures.append((label, setup_failed))

    # -- merged table --------------------------------------------------
    print("\n" + "=" * 62)
    print("MERGED REGRESSION SUMMARY")
    print("=" * 62)
    suite_names = [getattr(m, "NAME", m.__name__) for m in mods]
    failed = 0
    skipped_all: List[Tuple[str, str]] = []
    flaky_all: List[Tuple[str, str]] = []
    for name in suite_names:
        cells = []
        for label, _port in resolved:
            setup = next((s for l, s in setup_failures if l == label), None)
            if setup is not None:
                cells.append("%s:SETUP-FAIL" % label)
                continue
            entry = next(((r, f, s) for n, r, f, s in by_board.get(label, (None, [], []))[1]
                          if n == name), None)
            if entry is None:
                cells.append("%s:?" % label)
                continue
            res, _failed_names, skipped = entry
            if skipped or _is_skip(res):
                cells.append("%s:SKIP" % label)
                skipped_all.append((label, name))
            elif res.ok:
                cells.append("%s:PASS(%d)" % (label, res.passed))
            else:
                cells.append("%s:FAIL(%d/%d)" % (label, res.passed, res.total))
                failed += 1
        print("  %-30s %s" % (name[:30], "  ".join(cells)))
    for label, port, results, flaky, _setup in collected:
        for name in flaky:
            flaky_all.append((label, name))
    if skipped_all:
        print("\nSKIPPED (prerequisite unavailable; not a failure):")
        for label, name in skipped_all:
            print("  %s: %s" % (label, name))
    if flaky_all:
        print("\nFLAKY (passed on retry, quarantine for re-run):")
        for label, name in flaky_all:
            print("  %s: %s" % (label, name))
    if setup_failures:
        print("\nSETUP FAILURES (board never ran; gate is red):")
        for label, err in setup_failures:
            print("  %s: %s" % (label, err))
        failed += len(setup_failures)

    # -- failure details ------------------------------------------------
    details = []
    for label, _port in resolved:
        for name, res, failed_names, skipped in by_board.get(label, (None, [], []))[1]:
            if skipped or res.ok:
                continue
            detail = res.error or "; ".join(failed_names[:5])
            details.append((label, name, (detail or "")[:300]))
    if details:
        print("\nFAILURE DETAILS:")
        for label, name, detail in details:
            print("  %s:%s: %s" % (label, name, detail))

    # -- board-vs-board differential ------------------------------------
    print("\nBOARD DIFFERENTIAL (fails on exactly one board):")
    diff_found = False
    for name in suite_names:
        bad = [label for label, _port in resolved
               if _board_failed(by_board.get(label), name)]
        if 0 < len(bad) < len(resolved):
            diff_found = True
            good = [label for label, _port in resolved if label not in bad]
            print("  %s fails on %s, passes on %s"
                  % (name, ",".join(bad), ",".join(good)))
    if not diff_found:
        print("  none - boards agree on every suite")

    # -- slow-check against baseline ------------------------------------
    if baseline:
        print("\nSLOW CHECKS (vs baseline):")
        slow_found = False
        for label, _port in resolved:
            for name, res, _failed, skipped in by_board.get(label, (None, [], []))[1]:
                if skipped:
                    continue
                want = (baseline.get(label) or {}).get(name)
                if want and res.duration > max(want * 1.5, want + 20.0):
                    slow_found = True
                    print("  %s:%s %.0fs vs baseline %.0fs"
                          % (label, name, res.duration, want))
        if not slow_found:
            print("  none")

    print("\nRESULT %s" % ("OK" if failed == 0 else "FAIL"))
    return (1 if failed else 0,
            {label: by_board.get(label) for label, _p in resolved})


def _board_failed(entry, name: str) -> bool:
    if not entry:
        return False
    for n, res, _failed, skipped in entry[1]:
        if n == name:
            return (not skipped) and (not res.ok)
    return False


def _filter_tier(mods, tier: str):
    if tier == "fast":
        keep = set(TIER_FAST)
        return [m for m in mods if m.__name__.split(".")[-1] in keep]
    return mods


def _load_baseline(path: Optional[str]) -> Optional[Dict[str, Dict[str, float]]]:
    if not path:
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError) as exc:
        print("warning: cannot load baseline %s: %s" % (path, exc))
        return None


def _write_baseline(path: str, report: Dict) -> None:
    data: Dict[str, Dict[str, float]] = {}
    for label, entry in report.items():
        if not entry:
            continue
        per = {}
        for name, res, _failed, skipped in entry[1]:
            if not skipped and not _is_skip(res):
                per[name] = round(res.duration, 1)
        data[label] = per
    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("baseline written to %s" % path)
    except OSError as exc:
        print("warning: cannot write baseline %s: %s" % (path, exc))


def _self_test_summary() -> int:
    """Host-only check of the SKIP/PASS/FAIL summary verdict (no board).

    Guards the rules the runner must never get wrong: a prerequisite skip is
    SKIP (green), a zero-assertion suite is FAIL (no false green), and a
    genuine failure still fails the run.
    """
    cases = [
        ("pass", [SuiteResult("ok", 7, 0)], "PASS", 0, 0),
        ("skip", [SuiteResult("needs-usb", 0, 0, skipped=1)], "SKIP", 0, 1),
        # skipped>0 with a failure is a FAIL, never a silent skip.
        ("skip+fail", [SuiteResult("mixed", 0, 1, skipped=1)],
         "FAIL", 1, 0),
        # No assertions and no skip: a false green, so FAIL.
        ("vacuous", [SuiteResult("empty", 0, 0)], "FAIL", 1, 0),
    ]
    bad = 0
    for label, results, want_status, want_failed, want_skipped in cases:
        rows, failed, skipped = format_suite_summary(results)
        text = "\n".join(rows)
        ok = (rows and rows[0].split()[0] == want_status
              and failed == want_failed and skipped == want_skipped)
        print("%-4s summary %-10s -> %s (failed=%d skipped=%d)"
              % ("OK" if ok else "FAIL", label, rows[0].strip(), failed, skipped))
        if not ok:
            bad += 1
            print("     expected %s failed=%d skipped=%d, got:\n%s"
                  % (want_status, want_failed, want_skipped, text))
    # Clean-boot declaration: only suites that opt in demand a hard reset.
    plain = type("Plain", (), {})()
    opted = type("Opted", (), {"NEEDS_CLEAN_BOOT": True})()
    off = type("Off", (), {"NEEDS_CLEAN_BOOT": False})()
    boot_cases = [(plain, False), (opted, True), (off, False)]
    for mod, want in boot_cases:
        ok = needs_clean_boot(mod) == want
        print("%-4s NEEDS_CLEAN_BOOT %-6s -> %s" %
              ("OK" if ok else "FAIL", type(mod).__name__, needs_clean_boot(mod)))
        if not ok:
            bad += 1
    wedged = SuiteResult("w", 0, 1,
                         error="run('x'): marker not seen within 30s")
    healthy_fail = SuiteResult("h", 0, 1, error="check failed")
    for label, res, want in (("timeout", wedged, True),
                             ("plain failure", healthy_fail, False)):
        ok = suite_wedged(res) == want
        print("%-4s wedged detection %-14s -> %s"
              % ("OK" if ok else "FAIL", label, suite_wedged(res)))
        if not ok:
            bad += 1
    # The static gate list must stay in step with tools/harness/verify.py.
    want_gates = [("comment_index.py", ()), ("breadcrumbs.py", ()),
                  ("green_guard.py", ("--strict",)), ("check_assets.py", ()),
                  ("gallery_audit.py", ()), ("gallery_audit_selftest.py", ()),
                  ("led_contract.py", ())]
    for include_size, expected in ((False, want_gates),
                                   (True, want_gates + [("size_budget.py", ())])):
        got = [(spec[1], tuple(spec[2]) if len(spec) > 2 else ())
               for spec in static_gate_specs(include_size)]
        ok = got == expected
        print("%-4s static gates (size=%s)" %
              ("OK" if ok else "FAIL", include_size))
        if not ok:
            bad += 1
            print("     expected %r got %r" % (expected, got))
    # read_until's missing-marker canary is a host-only check too.
    if read_until_self_test() != 0:
        bad += 1
    if dogfood_self_test() != 0:
        bad += 1
    print("summary self-test %s" % ("OK" if bad == 0 else "FAIL"))
    return 1 if bad else 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="P4MiniShell host test runner")
    ap.add_argument("port", nargs="?", default=None, help="COMx (argv/P4_PORT/COM11)")
    ap.add_argument("--list", action="store_true", help="list suites and exit")
    ap.add_argument("--self-test", action="store_true",
                    help="host-only summary verdict check (no board)")
    ap.add_argument("--only", default=None, help="comma-separated suite names")
    ap.add_argument("--skip", default=None, help="comma-separated suite names")
    ap.add_argument("--tag", default=None, help="only suites with this tag")
    ap.add_argument("--quick", action="store_true", help="shorten soak/boot waits")
    ap.add_argument("--boards", default=None,
                    help="comma-separated COM ports or board slugs for a "
                         "concurrent multi-board gate (e.g. COM3,COM6)")
    ap.add_argument("--tier", default="full", choices=("fast", "full"),
                    help="fast: per-area smoke subset for feature delivery; "
                         "full: every suite for releases (implies --quick when fast)")
    ap.add_argument("--fail-fast", action="store_true", default=False,
                    help="stop remaining suites at the first failure")
    ap.add_argument("--keep-going", action="store_true", default=False,
                    help="never stop early (overrides fail-fast)")
    ap.add_argument("--strict-green", action="store_true",
                    help="a suite that passes only on retry is a failure")
    ap.add_argument("--retry", type=int, default=0,
                    help="retry a failed suite N times; pass-on-retry is FLAKY, never for panics")
    ap.add_argument("--baseline", default=None,
                    help="JSON {board: {suite: seconds}} to flag slow-check regressions")
    ap.add_argument("--write-baseline", default=None, metavar="PATH",
                    help="with --boards: record per-suite durations to PATH as a future --baseline")
    ap.add_argument("--guard", action="store_true",
                    help="run the static gates (false-green, comments, breadcrumbs) "
                         "before the hardware run; abort if any fails")
    ap.add_argument("--static", action="store_true",
                    help="run only the static gates (no hardware) and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test_summary()

    if args.static or args.guard:
        rc = run_static_gates()
        if args.static or rc != 0:
            return rc

    mods = discover()
    if args.list:
        for mod in mods:
            print("%-16s %s" % (mod.__name__.split(".")[-1], getattr(mod, "NAME", "")))
        return 0

    if args.only:
        wanted = {s.strip() for s in args.only.split(",")}
        mods = [m for m in mods if m.__name__.split(".")[-1] in wanted]
    if args.skip:
        drop = {s.strip() for s in args.skip.split(",")}
        mods = [m for m in mods if m.__name__.split(".")[-1] not in drop]
    if args.tag:
        mods = [m for m in mods if args.tag in getattr(m, "TAGS", [])]
    if not args.only and not args.tag:
        mods = _filter_tier(mods, args.tier)
    quick = args.quick or (args.tier == "fast"
                           and not args.only and not args.tag)

    if not mods:
        print("no suites selected")
        return 1
    fail_fast = args.fail_fast and not args.keep_going
    if args.boards:
        tokens = [t.strip() for t in args.boards.split(",") if t.strip()]
        if not tokens:
            print("no boards selected")
            return 1
        # A release gate stops everything at the first red board.
        rc, _report = run_boards(tokens, mods, quick=quick,
                                 fail_fast=fail_fast or (len(tokens) > 1 and not args.keep_going),
                                 retry=args.retry,
                                 strict_green=args.strict_green,
                                 baseline=_load_baseline(args.baseline))
        if args.write_baseline and _report:
            _write_baseline(args.write_baseline, _report)
        return rc
    return run_suites(mods, args.port, quick=quick,
                      keep_going=not fail_fast)
