# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Suite discovery and orchestration.

A suite is a module under ``tools/suites/`` exposing:

* ``NAME``      - human name shown in the report.
* ``TAGS``      - optional list (``"sd"``, ``"network"``, ``"slow"``, ...).
* ``run(dev, ctx)`` - returns a :class:`~p4test.asserts.Checklist` or
  :class:`~p4test.asserts.SuiteResult`.

The runner opens ONE device session, runs the selected suites in order, and
prints a single PASS/FAIL table with a non-zero exit on failure.
"""
from __future__ import annotations

import argparse
import importlib
import pkgutil
import sys
import time
import traceback
from typing import List, Optional

from .asserts import Checklist, SuiteResult
from .device import Device
from .session import PANICS, P4Error, PanicError


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


def run_suites(mods, port: Optional[str], quick: bool = False,
               keep_going: bool = True) -> int:
    results: List[SuiteResult] = []
    dev = Device(port, boot=True, settle_ms=4000 if quick else 8000)
    try:
        for mod in mods:
            name = getattr(mod, "NAME", mod.__name__)
            print("\n" + "=" * 62)
            print("SUITE: %s" % name)
            print("=" * 62)
            started = time.time()
            try:
                out = mod.run(dev, {"quick": quick})
                if isinstance(out, Checklist):
                    out.print_report()
                    results.append(out.result())
                elif isinstance(out, SuiteResult):
                    results.append(out)
                else:
                    results.append(SuiteResult(name, 0, 1,
                                               error="suite returned %r" % type(out)))
            except PanicError as exc:
                print("  [PANIC] %s" % exc)
                results.append(SuiteResult(name, 0, 1, duration=time.time() - started,
                                           error="PANIC: %s" % exc))
            except P4Error as exc:
                print("  [ERROR] %s" % exc)
                results.append(SuiteResult(name, 0, 1, duration=time.time() - started,
                                           error=str(exc)))
            except Exception as exc:  # noqa: BLE001
                traceback.print_exc()
                results.append(SuiteResult(name, 0, 1, duration=time.time() - started,
                                           error="%s: %s" % (type(exc).__name__, exc)))
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


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="P4MiniShell host test runner")
    ap.add_argument("port", nargs="?", default=None, help="COMx (argv/P4_PORT/COM11)")
    ap.add_argument("--list", action="store_true", help="list suites and exit")
    ap.add_argument("--only", default=None, help="comma-separated suite names")
    ap.add_argument("--skip", default=None, help="comma-separated suite names")
    ap.add_argument("--tag", default=None, help="only suites with this tag")
    ap.add_argument("--quick", action="store_true", help="shorten soak/boot waits")
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

    if not mods:
        print("no suites selected")
        return 1
    return run_suites(mods, args.port, quick=args.quick)
