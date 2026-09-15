#!/usr/bin/env python3
"""keyboard_test.py - HW verification for the OSK mode registry + `keyboard mode`.

Checks `keyboard status`, the `keyboard.page=<name>` round-trip for every
page (and the common aliases), a rejected unknown page, and restores the
letters page afterwards. Pure `keyboard_mode_name`/`keyboard_mode_parse` are
also unit-tested (test/main/test_keyboard.c); this driver proves the command
path and the LVGL page switch on the board.

Usage: python tools/keyboard_test.py [COMx]
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
            st = run_quiet(sh, "keyboard status")
            check("status line", st, "keyboard:")
            check("status external", st, "external=")
            check("default page", run_quiet(sh, "keyboard mode"),
                  "keyboard.page=text_lower")

            for page in ("text_lower", "text_upper", "number", "symbols",
                         "nav", "nav2"):
                out = run_quiet(sh, "keyboard mode " + page)
                check("set %s" % page, out, "keyboard.page=%s" % page)

            for alias, page in (("letters", "text_lower"),
                                ("caps", "text_upper"),
                                ("num", "number"),
                                ("special", "symbols"),
                                ("edit", "nav2")):
                out = run_quiet(sh, "keyboard mode " + alias)
                check("alias %s" % alias, out, "keyboard.page=%s" % page)

            check("reject unknown", run_quiet(sh, "keyboard mode bogus"),
                  "unknown page 'bogus'")

            # Leave the shell on the letters page.
            out = run_quiet(sh, "keyboard mode text_lower")
            check("restore letters", out, "keyboard.page=text_lower")

            show = run_quiet(sh, "keyboard show")
            check("show", show, "keyboard shown")
            hide = run_quiet(sh, "keyboard hide")
            check("hide", hide, "keyboard hidden")
            run_quiet(sh, "keyboard show")
        finally:
            sh.close()
    except Exception as exc:  # noqa: BLE001
        FAILS.append("exception")
        print("  [FAIL] exception: %s" % exc)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
