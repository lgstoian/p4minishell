#!/usr/bin/env python3
"""smoke_apps.py - reactive smoke-walk of the P4 reference apps.

Drives apps/adventure, apps/notes and apps/mood on-board the way
deep_test.py drives the Companion: markers are sync points, `N\\n` selects
list items, `ok\\n` dismisses dialogs, values feed `ask` after a settle.

Usage: python smoke_apps.py [COMx]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
import deep_test as D

ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEAP_RE = re.compile(r"heap (\d+)/(\d+) pct (\d+)")


def clean(text):
    return ANSI.sub("", text)


def log_text(st):
    return clean("".join(st.log))


def back_to_shell(ser, st):
    """Prove the app exited: the worker answers an echo with its own line."""
    ser.reset_input_buffer()
    st.buf = ""
    ser.write(b"echo SMOKEDONE\n")
    end = time.time() + 15
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            s = data.decode(errors="replace")
            st.buf += s
            st.log.append(s)
            if D.worker_line(clean(st.buf), "SMOKEDONE"):
                return True
        else:
            time.sleep(0.05)
    return False


def walk_advent(ser, st, results):
    def check(name, ok):
        results.append(("advent:" + name, ok))
        print("  advent %-12s %s" % (name, "PASS" if ok else "FAIL"))

    ser.reset_input_buffer()
    st.buf = ""
    ser.write(b"ADVENT.BAT\n")
    if not st.wait_for("[M-ADV]", 60):
        check("launch", False)
        return False
    check("launch", True)
    st.buf = ""
    if not D.press(ser, st, b"inv\n", "nothing yet"):
        check("inv", False)
        return False
    check("inv", True)
    # Silent moves (draw-only rooms): single settled sends, never press() —
    # a press retry would issue the step twice and mis-navigate.
    D.send_after_settle(ser, b"n\n", settle=7.0)  # courtyard -> hall
    D.send_after_settle(ser, b"w\n", settle=7.0)  # hall -> library
    st.buf = ""
    if not D.press(ser, st, b"take key\n", "brass key"):
        check("take-key", False)
        return False
    check("take-key", True)
    D.send_after_settle(ser, b"e\n", settle=7.0)  # library -> hall
    D.send_after_settle(ser, b"u\n", settle=7.0)  # hall -> tower
    st.buf = ""
    if not D.press(ser, st, b"take amulet\n", "amulet"):
        check("take-amulet", False)
        return False
    check("take-amulet", True)
    st.buf = ""
    if not D.press(ser, st, b"score\n", "Score: 30 in"):
        check("score", False)
        return False
    check("score", True)
    if not D.press(ser, st, b"quit\n", "[M-ADV-QUIT]"):
        check("quit", False)
        return False
    check("quit", True)
    if not back_to_shell(ser, st):
        check("back", False)
        return False
    check("back", True)
    return True


def walk_advent_win(ser, st, results):
    """Part 2: lantern + key + cellar darkness + save/load + win path."""
    def check(name, ok):
        results.append(("advent:" + name, ok))
        print("  advent %-12s %s" % (name, "PASS" if ok else "FAIL"))

    ser.reset_input_buffer()
    st.buf = ""
    ser.write(b"ADVENT.BAT\n")
    if not st.wait_for("[M-ADV]", 60):
        check("relaunch", False)
        return False
    D.send_after_settle(ser, b"e\n", settle=7.0)  # courtyard -> garden
    st.buf = ""
    if not D.press(ser, st, b"take lantern\n", "lantern. [+10]"):
        check("take-lantern", False)
        return False
    check("take-lantern", True)
    st.buf = ""
    # The Saved/Loaded dialogs are serial-silent; sync on the markers.
    if not D.press(ser, st, b"save\n", "[M-ADV-SAVED]"):
        check("save", False)
        return False
    check("save", True)
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss Saved dialog
    st.buf = ""
    if not D.press(ser, st, b"load\n", "[M-ADV-LOADED]"):
        check("load", False)
        return False
    check("load", True)
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss Loaded dialog
    D.send_after_settle(ser, b"w\n", settle=7.0)  # garden -> courtyard
    D.send_after_settle(ser, b"n\n", settle=7.0)  # courtyard -> hall
    D.send_after_settle(ser, b"w\n", settle=7.0)  # hall -> library
    st.buf = ""
    if not D.press(ser, st, b"take key\n", "brass key"):
        check("retake-key", False)
        return False
    D.send_after_settle(ser, b"e\n", settle=7.0)  # library -> hall
    D.send_after_settle(ser, b"e\n", settle=7.0)  # hall -> cellar (lit)
    st.buf = ""
    if not D.press(ser, st, b"use key\n", "[M-ADV-WIN]", timeout=60):
        check("win", False)
        return False
    check("win", True)
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss victory pause
    if not back_to_shell(ser, st):
        check("back", False)
        return False
    check("back", True)
    return True


def walk_notes(ser, st, results):
    def check(name, ok):
        results.append(("notes:" + name, ok))
        print("  notes  %-12s %s" % (name, "PASS" if ok else "FAIL"))

    ser.reset_input_buffer()
    st.buf = ""
    # Idempotent start: drop the store so leftover smoke records from an
    # interrupted run can never shift the ids this walk asserts on.
    ser.write(b"db drop zettel\n")
    time.sleep(4.0)
    ser.reset_input_buffer()
    ser.write(b"NOTES.BAT\n")
    if not st.wait_for("[M-NOTE]", 60):
        check("launch", False)
        return False
    check("launch", True)
    time.sleep(12.0)  # first run creates the db + a dialog; steady state: menu
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss dialog / ignored
    D.send_after_settle(ser, b"1\n", settle=8.0)  # New note -> ask key
    D.send_after_settle(ser, b"smoke1\n", settle=8.0)  # key -> category list
    D.send_after_settle(ser, b"2\n", settle=8.0)  # Permanent -> body choice
    D.send_after_settle(ser, b"1\n", settle=8.0)  # Type it -> ask body
    ser.write(b"hello zettel\n")
    if not st.wait_for("[M-NOTE-SAVED]", 60):
        check("add", False)
        return False
    check("add", True)
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss Saved dialog
    st.buf = ""  # drop any ask-echo of "smoke1" so the List match is fresh
    D.send_after_settle(ser, b"2\n", settle=8.0)  # List notes
    if not st.wait_for("smoke1", 45):
        check("list", False)
        return False
    check("list", True)
    m = re.search(r"(\d+)\.\s*\[\d+\]\s*smoke1", log_text(st))
    # The List ends in a `pause` pager that opens late; dismiss twice (a
    # stray newline is harmless: menus ignore non-numeric input and the
    # app's asks treat empty as cancel-to-menu).
    D.send_after_settle(ser, b"\n", settle=3.0)  # dismiss pause pager
    time.sleep(4.0)
    D.send_after_settle(ser, b"\n", settle=3.0)  # late-pager safety
    if not m:
        check("delete", False)
        return False
    D.send_after_settle(ser, b"5\n", settle=8.0)  # Delete note -> ask id
    D.send_after_settle(ser, (m.group(1) + "\n").encode(), settle=8.0)
    # Board text is `db: soft-deleted <id> in zettel` (lowercase); the
    # `Deleted <id>.` dialog follows it.
    if not st.wait_for("soft-deleted", 45):
        check("delete", False)
    else:
        check("delete", True)
    D.send_after_settle(ser, b"ok\n", settle=3.0)
    D.send_after_settle(ser, b"8\n", settle=5.0)  # Back
    if not back_to_shell(ser, st):
        check("back", False)
        return False
    check("back", True)
    return True


def walk_mood(ser, st, results):
    def check(name, ok):
        results.append(("mood:" + name, ok))
        print("  mood   %-12s %s" % (name, "PASS" if ok else "FAIL"))

    ser.reset_input_buffer()
    st.buf = ""
    ser.write(b"MOOD.BAT\n")
    if not st.wait_for("[M-MOOD]", 60):
        check("launch", False)
        return False
    check("launch", True)
    time.sleep(12.0)  # first modal after launch can take tens of seconds
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # ignored by the menu
    D.send_after_settle(ser, b"1\n", settle=8.0)  # Live mood, 5 samples
    # Discriminate a lost "1" from a hung console: a bare echo needs no
    # worker and must answer even while a modal is open.
    time.sleep(10.0)
    ser.write(b"echo PING\n")
    end = time.time() + 15
    ping = False
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            s = data.decode(errors="replace")
            st.buf += s
            st.log.append(s)
            if "PING" in clean(st.buf):
                ping = True
                break
        else:
            time.sleep(0.05)
    if not ping:
        check("console-dead", False)
        return False
    end = time.time() + 90
    samples = 0
    while time.time() < end and samples < 5:
        samples = log_text(st).count("[M-MOOD-SAMPLE]")
        time.sleep(1.0)
    if samples < 5:
        check("samples", False)
        return False
    check("samples", True)
    m = HEAP_RE.search(log_text(st))
    if not m or int(m.group(1)) == 0 or int(m.group(3)) > 100:
        check("heap-parse", False)
        return False
    check("heap-parse heap=%s/%s pct=%s" % m.groups(), True)
    print("  mood   heap live: free=%s total=%s pct=%s" % m.groups())
    D.send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss Sampling dialog
    D.send_after_settle(ser, b"5\n", settle=5.0)  # Back
    if not back_to_shell(ser, st):
        check("back", False)
        return False
    check("back", True)
    return True


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = D.open_noreset(port)
    time.sleep(0.5)
    ser.reset_input_buffer()
    if not D.shell_up(ser):
        print("FAIL: shell not responding")
        ser.close()
        return 1
    time.sleep(15.0)  # boot background work quiesces (shared SDMMC)
    ser.reset_input_buffer()
    st = D.Stream(ser)

    results = []
    print("== adventure ==")
    walk_advent(ser, st, results)
    print("== adventure/win ==")
    walk_advent_win(ser, st, results)
    print("== notes ==")
    walk_notes(ser, st, results)
    print("== mood ==")
    walk_mood(ser, st, results)

    # Best-effort cleanup of smoke artifacts (no verdicts).
    print("== cleanup ==")
    try:
        ser.reset_input_buffer()
        for cmd in (b"del /p ADVENT.SAV\n", b"del /p PROBE33.BAT\n",
                    b"db purge zettel\n"):
            ser.write(cmd)
            time.sleep(3.0)
        ser.read(ser.in_waiting or 1)
        print("  cleanup sent")
    except Exception as e:
        print("  cleanup skipped: %r" % e)

    ser.close()
    fails = [n for n, ok in results if not ok]
    print("SMOKE %d/%d %s" % (len(results) - len(fails), len(results),
                              "PASS" if not fails else "FAIL %s" % fails))
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "smoke_apps.log"), "w") as f:
        f.write("".join(st.log))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
