#!/usr/bin/env python3
"""pkg_test.py - HW round-trip for `pkg` (B1 packaged SD apps).

Precondition: `python apps/push_pkgs.py COMx` has pushed PKGS/PKGTEST.
Exercises list/info/verify/install/run/remove and asserts the installed files
appear and disappear.
"""
import sys
import time

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
        print("      out: %r" % out[-400:])


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    try:
        run_quiet(sh, "pkg remove PKGTEST")  # clean slate

        print("-- preconditions")
        check("not listed", run_quiet(sh, "pkg list"), "PKGTEST", want=False)
        check("no manifest", run_quiet(sh, "pkg verify PKGTEST"),
              "no manifest", want=True)

        print("-- install")
        out = run_quiet(sh, "pkg install PKGTEST")
        check("payload copied", out, "pkg: installed PKGTEST.BAT")
        check("installed", out, "pkg: PKGTEST installed")

        print("-- installed state")
        check("listed", run_quiet(sh, "pkg list"), "PKGTEST")
        check("verify ok", run_quiet(sh, "pkg verify PKGTEST"), "1/1 ok")
        info = run_quiet(sh, "pkg info PKGTEST")
        check("title", info, "Package Test")
        check("version", info, "1.0")

        print("-- runs")
        run = run_quiet(sh, "PKGTEST.BAT")
        check("marker run", run, "[M-PKGTEST]")
        check("marker done", run, "[M-PKGTEST-DONE]")

        print("-- check all")
        check("check sees it", run_quiet(sh, "pkg check", timeout=45), "PKGTEST")

        print("-- remove")
        out = run_quiet(sh, "pkg remove PKGTEST")
        check("uninstalled", out, "uninstalled")
        check("not listed after", run_quiet(sh, "pkg list"), "PKGTEST", want=False)
        check("no manifest after", run_quiet(sh, "pkg verify PKGTEST"),
              "no manifest")
        check("no info after", run_quiet(sh, "pkg info PKGTEST"),
              "(missing)")
    finally:
        sh.close()

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
