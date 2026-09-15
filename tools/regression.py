#!/usr/bin/env python3
"""regression.py - one-command host regression for a release.

Runs the host-side suites and guards against whatever firmware is currently
flashed, prints a single PASS/FAIL table, and exits non-zero on any failure.
Reuses the individual drivers (do not reimplement their logic here).

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
APPS = os.path.join(ROOT, "apps")

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
        ("companion deep", [sys.executable, os.path.join(APPS, "companion", "deep_test.py"), port], "DEEP PASS", 1200),
        ("companion db", [sys.executable, os.path.join(APPS, "companion", "db_test.py"), port], "DB PASS", 1200),
        ("companion alarm", [sys.executable, os.path.join(APPS, "companion", "alarm_test.py"), port], "ALARM PASS", 1200),
        ("app smoke", [sys.executable, os.path.join(APPS, "smoke_apps.py"), port], "SMOKE 21/21 PASS", 1200),
        ("pak install/remove", [sys.executable, os.path.join(TOOLS, "pkg_test.py"), port], "RESULT OK", 900),
        ("theme", [sys.executable, os.path.join(TOOLS, "theme_test.py"), port], "RESULT OK", 900),
        ("gfx toolkit", [sys.executable, os.path.join(TOOLS, "gfx_toolkit_test.py"), port], "RESULT OK", 900),
        ("plot", [sys.executable, os.path.join(TOOLS, "plot_test.py"), port], "RESULT OK", 900),
        ("header", [sys.executable, os.path.join(TOOLS, "header_test.py"), port], "RESULT OK", 900),
        ("keyboard", [sys.executable, os.path.join(TOOLS, "keyboard_test.py"), port], "RESULT OK", 900),
        ("editor", [sys.executable, os.path.join(TOOLS, "editor_test.py"), port], "RESULT OK", 900),
        ("editor large", [sys.executable, os.path.join(TOOLS, "editor_large_test.py"), port, "4000"], "RESULT OK", 900),
        ("timer", [sys.executable, os.path.join(TOOLS, "timer_test.py"), port], "RESULT OK", 900),
        ("csv", [sys.executable, os.path.join(TOOLS, "csv_test.py"), port], "RESULT OK", 900),
        ("export", [sys.executable, os.path.join(TOOLS, "export_test.py"), port], "RESULT OK", 900),
        ("bind", [sys.executable, os.path.join(TOOLS, "bind_test.py"), port], "RESULT OK", 900),
        ("crypt", [sys.executable, os.path.join(TOOLS, "crypt_test.py"), port], "RESULT OK", 900),
        ("tcpterm", [sys.executable, os.path.join(TOOLS, "tcpterm_test.py"), port], "RESULT OK", 900),
        ("userial", [sys.executable, os.path.join(TOOLS, "userial_test.py"), port], "RESULT OK", 900),
        ("ui touch", [sys.executable, os.path.join(TOOLS, "ui_touch_test.py"), port], "RESULT OK", 900),
        ("completion", [sys.executable, os.path.join(TOOLS, "completion_test.py"), port], "RESULT OK", 900),
        ("tx stress", [sys.executable, os.path.join(TOOLS, "tx_stress_test.py"), port, "200"], "RESULT OK", 600),
        ("boot regression", [sys.executable, os.path.join(TOOLS, "boot_regression.py"), port, str(boots)], "RESULT OK", 400 + boots * 25),
    ]

    results = []
    for name, cmd, expect, timeout in steps:
        print("== %s ==" % name, flush=True)
        # Fresh shell: some drivers (gfx/TUI/header) leave the board in a mode
        # that breaks the next one, and open_port() no longer resets on open.
        hard_reset(port)
        time.sleep(1.0)
        t0 = time.time()
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            out = (proc.stdout or "") + (proc.stderr or "")
            ok = expect in out
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
