#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""theme_test.py - HW verification for the B3 theme switching + persistence.

Checks the CLI (list/show/set), a rejected unknown theme, that /save survives
a reboot (SHELL.INI), and restores the default afterwards.

Usage: python tools/theme_test.py [COMx]
"""
import sys

sys.path.insert(0, "tools")
from bg_run import boot, run_quiet  # noqa: E402

FAILS = []


def check(name, out, needle, want=True):
    got = needle in out
    ok = got == want
    print("  [%s] %s (%s%r)" % ("PASS" if ok else "FAIL", name,
                                "" if want else "not ", needle))
    if not ok:
        FAILS.append(name)
        print("      out: %r" % out[-300:])


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    try:
        sh = boot(port)
        try:
            lst = run_quiet(sh, "theme list")
            for name in ("default", "amber", "ice", "mono"):
                check("list %s" % name, lst, name)
            check("active default", run_quiet(sh, "theme show"), "default (active)")
            check("reject unknown", run_quiet(sh, "theme set bogus"),
                  "unknown theme 'bogus'")
            out = run_quiet(sh, "theme set amber /save")
            check("set amber saved", out, "-> amber (saved)")
            check("show amber", run_quiet(sh, "theme show"), "amber (active)")
            # Colors are 0xRRGGBB in the table output.
            check("amber accent", run_quiet(sh, "theme show amber"), "accent=FFB000")
        finally:
            sh.close()

        # Reboot: the saved theme must restore from SHELL.INI.
        sh = boot(port)
        try:
            check("persisted amber", run_quiet(sh, "theme show"), "amber (active)")
            out = run_quiet(sh, "theme set default /save")
            check("restore default", out, "-> default (saved)")
        finally:
            sh.close()
    except Exception as exc:  # noqa: BLE001
        FAILS.append("exception")
        print("  [FAIL] exception: %s" % exc)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
