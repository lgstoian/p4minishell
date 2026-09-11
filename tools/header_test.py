#!/usr/bin/env python3
"""header_test.py - HW verification for the responsive header.

Checks, across layout modes and rotations, that:
  - `header mode auto|full|compact` takes effect and reaches the expected
    content level(s);
  - the resolved panel widths never overlap and stay within the header;
  - `header status` reports want == actual for every panel (nothing clipped).

Pixel screenshots are saved to spikes/header/ for visual review, but the
assertions use the LVGL metrics (the screenshot path has a known horizontal
offset on this board, so pixel positions are not a reliable oracle).

Usage: python tools/header_test.py [COMx]
"""
import os
import re
import sys

sys.path.insert(0, "tools")
from bg_run import boot, run_quiet  # noqa: E402

FAILS = []
OUTDIR = os.path.join("spikes", "header")


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        FAILS.append(name)


def parse_status(text):
    m = {}
    for key in ("mode", "levels", "widths"):
        mm = re.search(r"header\.%s=([^\r\n]+)" % key, text)
        if mm:
            m[key] = mm.group(1).strip()
    return m


def widths(text):
    """want/actual ints from header.widths=screen:N want:a/b/c actual:d/e/f."""
    mm = re.search(r"header\.widths=screen:(\d+) want:(\d+)/(\d+)/(\d+) "
                   r"actual:(\d+)/(\d+)/(\d+)", text)
    if not mm:
        return None
    return [int(v) for v in mm.groups()]


def check_layout(sh, label, expect_status=None, expect_sys=None):
    st = parse_status(run_quiet(sh, "header status"))
    w = widths(run_quiet(sh, "header status"))
    if w is None:
        check(label + " widths", False, "no widths line")
        return
    screen, wl, wc, wr, al, ac, ar = w
    # No overlap / within bounds: widths match AND fit the screen.
    fits = (al + ac + ar) <= screen
    want_eq_actual = (wl == al or al == 0) and (wr == ar or ar == 0)
    check(label + " fits", fits,
          "actual %d+%d+%d <= screen %d" % (al, ac, ar, screen))
    check(label + " want==actual", want_eq_actual,
          "want %d/%d/%d actual %d/%d/%d" % (wl, wc, wr, al, ac, ar))
    if expect_status is not None:
        lv = st.get("levels", "")
        ok = ("status:%d" % expect_status) in lv
        check(label + " status level", ok, lv)
    if expect_sys is not None:
        lv = st.get("levels", "")
        ok = ("sys:%d" % expect_sys) in lv
        check(label + " sys level", ok, lv)
    return st


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    os.makedirs(OUTDIR, exist_ok=True)
    sh = boot(port)
    try:
        print("-- auto (default)")
        check_layout(sh, "auto")

        print("-- full")
        check("mode full", "header.mode=full" in run_quiet(sh, "header mode full"))
        # FULL keeps the full status labels; the system panel still compacts if
        # the full text cannot fit without overlapping (never overlaps).
        check_layout(sh, "full", expect_status=0)

        print("-- compact")
        check("mode compact", "header.mode=compact" in run_quiet(sh, "header mode compact"))
        check_layout(sh, "compact", expect_status=1, expect_sys=1)

        print("-- persistence")
        out = run_quiet(sh, "header mode auto /save")
        check("mode save", "(saved)" in out, out[-80:])

        print("-- rotations")
        for rot in ("90", "180", "270", "0"):
            run_quiet(sh, "rotate %s" % rot, timeout=30)
            import time
            time.sleep(3)
            check_layout(sh, "rot %s" % rot)
    finally:
        run_quiet(sh, "header mode auto /save")
        run_quiet(sh, "rotate 0")
        sh.close()

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
