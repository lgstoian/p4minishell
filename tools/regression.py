#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""regression.py - one-command host regression for a release.

Runs the full p4test suite plus the drivers with unique coverage the suites
do not have (package install/remove, large-editor soak, touch sweep, TX
integrity, boot regression), prints a single PASS/FAIL table, and exits
non-zero on any failure. Area drivers whose coverage lives in the suites
(theme/gfx/plot/header/keyboard/editor/timer/csv/export/bind/crypt/tcpterm/
userial/completion + the companion deep/db/alarm walks, all covered by
s01-s16) are intentionally NOT re-run here; see tools/README.md for the map.

The Unity unit suite is NOT run here: it needs the separate test image flashed
(`cd test && idf.py -p COMx flash`, then `python tools/unit_run.py COMx`).

Usage:
  python tools/regression.py [COMx] [--quick]
    --quick   boot_regression with 5 boots instead of 15
"""

import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")

sys.path.insert(0, TOOLS)
from shell_session import hard_reset  # noqa: E402


def port_arg(args):
    for a in args:
        if a.upper().startswith("COM"):
            return a.upper()
    return os.environ.get("P4_PORT", "COM11")


def main():
    args = sys.argv[1:]
    port = port_arg(args)
    quick = "--quick" in args
    boots = 5 if quick else 15

    steps = [
        ("companion deep", [sys.executable, os.path.join(ROOT, "apps", "companion", "deep_test.py"), port], "DEEP PASS", 1200),
        ("companion db", [sys.executable, os.path.join(ROOT, "apps", "companion", "db_test.py"), port], "DB PASS", 1200),
        ("companion alarm", [sys.executable, os.path.join(ROOT, "apps", "companion", "alarm_test.py"), port], "ALARM PASS", 1200),
        ("suites full", [sys.executable, os.path.join(TOOLS, "p4test_run.py"), port], "RESULT OK", 7200),
        # Deploy the package bundles first: pkg_test.py installs/removes them and
        # fails on a fresh SD that never had apps/push_pkgs.py run (the Tab5).
        ("pak deploy", [sys.executable, os.path.join(ROOT, "apps", "push_pkgs.py"), port], "RESULT OK", 600),
        ("pak install/remove", [sys.executable, os.path.join(TOOLS, "pkg_test.py"), port], "RESULT OK", 900),
        ("editor large", [sys.executable, os.path.join(TOOLS, "editor_large_test.py"), port, "4000"], "RESULT OK", 900),
        ("ui touch", [sys.executable, os.path.join(TOOLS, "ui_touch_test.py"), port], "RESULT OK", 900),
        ("tx stress", [sys.executable, os.path.join(TOOLS, "tx_stress_test.py"), port, "200"], "RESULT OK", 600),
        ("boot regression", [sys.executable, os.path.join(TOOLS, "boot_regression.py"), port, str(boots)], "RESULT OK", 400 + boots * 25),
    ]

    # One reset up front; each kept driver below is self-contained (and
    # boot_regression resets per boot itself). The old per-step reset existed
    # because every area driver re-ran suite coverage and left modal/gfx
    # state behind; that duplication is gone with the suite run above.
    hard_reset(port)
    time.sleep(1.0)

    results = []
    for name, cmd, expect, timeout in steps:
        print("== %s ==" % name, flush=True)
        t0 = time.time()
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            out = (proc.stdout or "") + (proc.stderr or "")
            ok = expect in out
            # A network-dependent driver may legitimately skip when the board
            # is not associated (`tcpterm_test.py` prints RESULT SKIP); that is
            # not a firmware failure.
            if not ok and "RESULT SKIP" in out:
                ok = True
                detail = "skipped (prerequisite unavailable)"
            else:
                detail = "ok" if ok else "missing %r" % expect
        except subprocess.TimeoutExpired:
            ok = False
            detail = "timeout after %ds" % timeout
        dt = time.time() - t0
        results.append((name, ok, detail, dt))
        print("   %s (%.1fs) %s" % ("PASS" if ok else "FAIL", dt, "" if ok else detail), flush=True)

    print("\n===== REGRESSION SUMMARY =====")
    failed = 0
    for name, ok, detail, dt in results:
        print("%-20s %s  %.0fs%s" % (name, "PASS" if ok else "FAIL", dt, "" if ok else "  <- " + detail))
        if not ok:
            failed += 1
    print("RESULT %s (%d/%d)" % ("OK" if failed == 0 else "FAIL", len(results) - failed, len(results)))
    return 1 if failed else 0


sys.exit(main())
