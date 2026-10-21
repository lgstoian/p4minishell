# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Suite discovery and orchestration.

A suite is a module under ``tools/suites/`` exposing:

* ``NAME``      - human name shown in the report.
* ``TAGS``      - optional list (``"sd"``, ``"network"``, ``"slow"``, ...).
* ``run(dev, ctx)`` - returns a :class:`~p4test.asserts.Checklist` or
  :class:`~p4test.asserts.SuiteResult`.

The runner opens ONE device session per board, runs the selected suites in
order, and prints a single PASS/FAIL table with a non-zero exit on failure.
With ``--boards`` several boards run concurrently (one thread each) and the
report merges per-board results, flags board-only failures, and (under
fail-fast) stops the other boards at the first failure.
"""
from __future__ import annotations

import argparse
import importlib
import io
import json
import pkgutil
import re
import sys
import threading
import time
import traceback
from typing import Dict, List, Optional, Tuple

from .asserts import Checklist, SuiteResult
from .device import Device
from .local_config import wifi_credentials
from .session import PANICS, P4Error, PanicError

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
        print("  [PANIC] %s" % exc)
        return (SuiteResult(name, 0, 1, duration=time.time() - started,
                            error="PANIC: %s" % exc),
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


def run_suites(mods, port: Optional[str], quick: bool = False,
               keep_going: bool = True) -> int:
    results: List[SuiteResult] = []
    dev = Device(port, boot=True, settle_ms=4000 if quick else 8000)
    ctx = {"quick": quick, "wifi": wifi_credentials()}
    try:
        for mod in mods:
            name = getattr(mod, "NAME", mod.__name__)
            print("\n" + "=" * 62)
            print("SUITE: %s" % name)
            print("=" * 62)
            res, _failed = _run_one_suite(mod, dev, ctx)
            results.append(res)
            if not keep_going and results and not results[-1].ok:
                break
    finally:
        dev.close()

    print("\n" + "=" * 62)
    print("REGRESSION SUMMARY")
    print("=" * 62)
    failed = 0
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        if not r.ok:
            failed += 1
        extra = (" - %s" % r.error) if r.error else ""
        print("  %-4s %-34s %3d/%3d checks%s"
              % (status, r.name[:34], r.passed, r.total, extra))
    total = len(results)
    print("\nRESULT %s (%d/%d suites)" % ("OK" if failed == 0 else "FAIL",
                                          total - failed, total))
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
                  retry: int, out: list, router: _ThreadRouter) -> None:
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
            for mod in mods:
                name = getattr(mod, "NAME", mod.__name__)
                if stop_event.is_set():
                    print("\nSUITE [%s]: %s SKIPPED (fail-fast)"
                          % (label, name))
                    board_results.append(
                        (name, SuiteResult(name, 0, 0, skipped=1), [], True))
                    continue
                print("\nSUITE [%s]: %s" % (label, name))
                # Long suites accumulate live UI/transcript/service state in
                # internal RAM (PSRAM is plentiful). When the internal heap is
                # too small for the next area (notably httpd's 8 KB task), a
                # clean boot is faster and more representative than failing
                # on fragmentation. SD state persists across the reset.
                if mod is not mods[0]:
                    try:
                        mem = dev.run("mem", timeout=20)
                        internal = re.search(r"mem\.internal:\s*(\d+)", mem)
                        dma = re.search(r"mem\.dma\.free:\s*(\d+)", mem)
                        low_internal = (internal is not None and
                                        int(internal.group(1)) < (12 * 1024))
                        low_dma = (dma is not None and
                                   int(dma.group(1)) < (9 * 1024))
                        # These areas need a healthy DMA/internal pool up
                        # front: I2S descriptors (power-audio) and httpd's
                        # 8 KB task/sockets (browser/http). Always start them
                        # from a clean boot, exactly like a user power-cycle.
                        force_boot = name in (
                            "editor", "input-ui", "power-audio", "apps",
                            "browser", "http")
                        if force_boot or low_internal or low_dma:
                            print("  [MEM] internal %s B / dma %s B free; "
                                  "rebooting before %s"
                                  % (internal.group(1) if internal else "?",
                                     dma.group(1) if dma else "?", name))
                            dev.reboot(settle_ms=4000 if quick else 8000)
                    except PanicError:
                        raise
                    except Exception:  # noqa: BLE001
                        pass
                attempts = 1 + max(0, retry)
                res, failed = _run_one_suite(mod, dev, ctx)
                tried = 1
                while not res.ok and tried < attempts and not (
                        res.error or "").startswith("PANIC"):
                    tried += 1
                    print("  [RETRY %d/%d] %s" % (tried, attempts, name))
                    res, failed = _run_one_suite(mod, dev, ctx)
                if tried > 1 and res.ok:
                    flaky.append(name)
                    print("  [FLAKY] %s passed on attempt %d" % (name, tried))
                board_results.append((name, res, failed, False))
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
               fail_fast: bool = True, retry: int = 0,
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
                                        fail_fast, retry, collected, router),
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
            if skipped:
                cells.append("%s:SKIP" % label)
            elif res.ok:
                cells.append("%s:PASS(%d)" % (label, res.passed))
            else:
                cells.append("%s:FAIL(%d/%d)" % (label, res.passed, res.total))
                failed += 1
        print("  %-30s %s" % (name[:30], "  ".join(cells)))
    for label, port, results, flaky, _setup in collected:
        for name in flaky:
            flaky_all.append((label, name))
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
            if not skipped:
                per[name] = round(res.duration, 1)
        data[label] = per
    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("baseline written to %s" % path)
    except OSError as exc:
        print("warning: cannot write baseline %s: %s" % (path, exc))


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="P4MiniShell host test runner")
    ap.add_argument("port", nargs="?", default=None, help="COMx (argv/P4_PORT/COM11)")
    ap.add_argument("--list", action="store_true", help="list suites and exit")
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
    ap.add_argument("--retry", type=int, default=0,
                    help="retry a failed suite N times; pass-on-retry is FLAKY, never for panics")
    ap.add_argument("--baseline", default=None,
                    help="JSON {board: {suite: seconds}} to flag slow-check regressions")
    ap.add_argument("--write-baseline", default=None, metavar="PATH",
                    help="with --boards: record per-suite durations to PATH as a future --baseline")
    args = ap.parse_args(argv)

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
                                 fail_fast=fail_fast or len(tokens) > 1,
                                 retry=args.retry,
                                 baseline=_load_baseline(args.baseline))
        if args.write_baseline and _report:
            _write_baseline(args.write_baseline, _report)
        return rc
    return run_suites(mods, args.port, quick=quick,
                      keep_going=not fail_fast)
