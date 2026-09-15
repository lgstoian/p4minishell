#!/usr/bin/env python3
"""editor_test.py - HW verification for the touch-complete `edit` surface.

The editor is a modal surface driven by serial lines while it is open, so this
driver writes raw lines (a `run_quiet` echo would be typed into the document).
It proves the new File > Open round-trip end to end (Open creates the named
buffer and later saves go to the NEW path, not the old one) and smoke-tests the
four commands promoted onto the Nav page (Comment/Match/Wrap/Reload), then
checks the shell is restored.

The OSK auto-Nav and page-switch behavior is proven by unit tests
(test/main/test_editor.c, test_keyboard.c) plus a screenshot; it cannot be
queried over serial while the editor owns the input.

Usage: python tools/editor_test.py [COMx]
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot  # noqa: E402
import bg_run  # noqa: E402

D = bg_run.D
PANICS = bg_run.PANICS
FAILS = []


def check(name, out, needle, want=True):
    got = needle in out
    ok = got == want
    print("  [%s] %s (%s%r)" % ("PASS" if ok else "FAIL", name,
                                "" if want else "not ", needle))
    if not ok:
        FAILS.append(name)
        print("      out: %r" % out[-400:])


def send(sh, line, wait=2.0):
    sh.s.write((line + "\r\n").encode())
    time.sleep(wait)
    out = D.read_all(sh.s, 0.4)
    for p in PANICS:
        if p.decode(errors="replace") in out:
            FAILS.append("panic after %s" % line)
    return out


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    stamp = time.strftime("%H%M%S")
    one = "_kt_%s_a.txt" % stamp
    two = "_kt_%s_b.txt" % stamp
    batch = "_kt_%s.bat" % stamp

    sh = boot(port)
    try:
        # 1) Create a scratch file through the editor.
        send(sh, "edit " + one, 6.0)
        send(sh, "ONEDATA", 2.0)
        send(sh, "\\s", 4.0)
        send(sh, "\\q", 4.0)
        send(sh, "y", 2.5)  # answer a quit-confirm if the save failed
        time.sleep(1.5)

        # 2) Start from an unnamed buffer (so the Open prompt is empty, not
        #    pre-filled with the current path) and File > Open a second file.
        send(sh, "edit", 6.0)
        send(sh, "\\open", 3.0)
        send(sh, two, 4.0)
        send(sh, "TWODATA", 2.0)
        send(sh, "\\s", 4.0)
        send(sh, "\\q", 4.0)
        send(sh, "y", 2.5)
        time.sleep(1.5)

        # 3) Verify: the new file got the text, the old file was untouched.
        out2 = D.ansi_strip(send(sh, "type " + two, 3.0))
        check("open created new file", out2, "TWODATA")
        out1 = D.ansi_strip(send(sh, "type " + one, 3.0))
        check("open kept old file", out1, "ONEDATA")

        # 4) Smoke the four Nav-page commands promoted in this change.
        send(sh, "edit " + batch, 6.0)
        send(sh, "ECHO HI", 2.0)
        for verb in ("\\co", "\\w", "\\b"):
            send(sh, verb, 1.5)
        send(sh, "\\l", 2.5)
        send(sh, "\\q", 4.0)
        send(sh, "y", 2.5)
        time.sleep(1.5)

        # 5) Shell restored and functional.
        out = D.ansi_strip(send(sh, "echo EDITOR_TEST_DONE", 3.0))
        check("shell restored", out, "EDITOR_TEST_DONE")
    except Exception as exc:  # noqa: BLE001
        FAILS.append("exception")
        print("  [FAIL] exception: %s" % exc)
    finally:
        sh.close()

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
