#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""completion_test.py - HW verification for shell completion + recall history.

Covers the batch-testable `history /search <text>` filter (hit, miss, and the
resulting ERRORLEVEL), the debounced autosave profile written to HISTORY.TXT,
and the inline ghost completion driven through the on-screen keyboard (`ui key`
plus the `ui state` ghost= field). The pure candidate/tokenizer and search
filter helpers are unit-tested in test/main/test_completion.c and
test/main/test_history_search.c; this driver proves the on-device wiring.

Usage: python tools/completion_test.py [COMx]
"""
import re
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot, run_quiet  # noqa: E402

FAILS = []


def ok(name, cond, detail=""):
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           (" (%s)" % detail) if detail else ""))
    if not cond:
        FAILS.append(name)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    try:
        sh = boot(port)
        try:
            run_quiet(sh, "history /clear")
            run_quiet(sh, "echo CT_ALPHA_ONE")
            run_quiet(sh, "echo CT_BETA_TWO")

            hit = run_quiet(sh, "history /search CT_ALPHA")
            ok("search hit lists entry", "CT_ALPHA_ONE" in hit, hit[-200:].strip())
            ok("search hit ERRORLEVEL 0",
               "CT_EL=0" in run_quiet(sh, "echo CT_EL=%ERRORLEVEL%"))

            miss = run_quiet(sh, "history /search CT_ZZZ_NOPE")
            ok("search miss reports no match", "no match" in miss)
            ok("search miss ERRORLEVEL 1",
               "CT_EL=1" in run_quiet(sh, "echo CT_EL=%ERRORLEVEL%"))

            # The 5 s debounce timer flushes the recall ring to the profile.
            time.sleep(7)
            saved = run_quiet(sh, "type HISTORY.TXT")
            ok("autosave profile has entries", "CT_ALPHA_ONE" in saved)

            # Inline ghost completion: the OSK feeds the input line, so the
            # `ui state` ghost= field must show the muted completion suffix.
            run_quiet(sh, "keyboard show")
            run_quiet(sh, "ui key c")
            run_quiet(sh, "ui key o")
            state = run_quiet(sh, "ui state")
            m = re.search(r"ghost=(\S+)", state)
            ghost = m.group(1) if m else ""
            ok("typed prefix reflected", "input=PS /sdcard> co" in state, state.strip())
            ok("ghost suffix shown", len(ghost) > 0, "ghost=%r" % ghost)
        finally:
            sh.close()
    except Exception as exc:  # noqa: BLE001
        FAILS.append("exception")
        print("  [FAIL] exception: %s" % exc)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
