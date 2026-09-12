#!/usr/bin/env python3
"""alarm_test.py - on-board verification of the `alarm`/`cal` commands.

Runs a clean sequence through the alarm surface and verifies the background
checker actually fires an alarm (marks it fired in the store). Output is
written to stdout; run with output redirection to a file for stable results.

Usage: python alarm_test.py [COMx]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from shell_session import open_port  # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def worker_line(text, word):
    """True only if the worker actually printed `word` as its own output
    line. The submit-side echo (`PS \\> echo WORD`) contains the word too,
    so substring matching false-passes on a wedged worker (or breaks a
    barrier early on a slow one). One leading `PS ...>` prompt prefix is
    stripped first: prompt-glue (trailing prompt + output on one serial
    line, normal under load) must not false-negative."""
    for ln in text.splitlines():
        s = ANSI.sub("", ln).strip()
        s = re.sub(r"^PS \S*> ", "", s).strip()
        if s == word and ("echo %s" % word) not in ANSI.sub("", ln):
            return True
    return False


def read_all(ser, seconds):
    end = time.time() + seconds
    out = []
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            out.append(ANSI.sub("", data.decode(errors="replace")))
    return "".join(out)


def wait(ser, needle, timeout=8):
    buf = ""
    end = time.time() + timeout
    while time.time() < end:
        d = read_all(ser, 0.25)
        if d:
            buf += d
            if needle in buf:
                return buf
    return buf


def shell_up(ser):
    # Requires the worker's own `ready` output line, not the `echo ready`
    # input echo (which the console task prints even while wedged).
    for _ in range(60):
        ser.write(b"echo ready\n")
        b = read_all(ser, 3.0)
        if worker_line(b, "ready") and "PS " in b and "> " in b:
            return True
        time.sleep(0.5)
    return False


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not shell_up(ser):
        print("FAIL: shell not responding")
        ser.close()
        return 1

    # Boot background work (Wi-Fi/SDIO bring-up) contends the shared SDMMC
    # bus for ~15s after the shell first answers; let it quiesce so the
    # first measured commands cannot time out spuriously.
    time.sleep(15.0)
    ser.reset_input_buffer()

    results = []
    tag = [0]

    def act(name, cmdline, expect, cap=60):
        # Barrier-sync like db_test: queue `echo DONE-n` behind the command
        # so its marker frames this command's output on the FIFO worker no
        # matter how slow the SD card is (a slow command otherwise shifts
        # every later check by one window and cascades false FAILs).
        tag[0] += 1
        marker = "DONE-%d" % tag[0]
        ser.reset_input_buffer()
        ser.write((cmdline + "\n").encode())
        ser.write(("echo %s\n" % marker).encode())
        buf = b""
        end = time.time() + cap
        while time.time() < end:
            data = ser.read(ser.in_waiting or 1)
            if data:
                buf += data
                # Break on the worker's marker OUTPUT line, not the
                # submit-side `echo DONE-n` input echo (see db_test).
                if worker_line(buf.decode(errors="replace"), marker):
                    break
            else:
                time.sleep(0.05)
        b = ANSI.sub("", buf.decode(errors="replace"))
        ok = (expect in b) and worker_line(b, marker)
        results.append((name, ok))
        print("%-42s %s" % (cmdline, "PASS" if ok else "FAIL(%s)" % expect))
        if not ok:
            print("      ", repr(b[-150:]))
        return b

    def cmdline(cmd, to=3):
        ser.write((cmd + "\n").encode())
        return read_all(ser, to)

    # clean slate
    act("del all", "alarm del all", "deleted")
    act("purge", "alarm purge", "purged")
    act("status empty", "alarm status", "alarm.count")

    # add future events (far future, so the checker leaves them alone)
    act("add standup", 'alarm add 2030-06-01 07:15 Standup /msg:Team call /beep /led', "scheduled")
    act("add weekly", "alarm add 2030-06-02 08:00 Weekly /weekly:0x7F", "scheduled")
    act("add silent", "alarm add 2030-06-03 09:00 Quiet /silent", "scheduled")

    # list
    act("list standup", "alarm list", "Standup")
    act("list weekly", "alarm list", "Weekly")
    act("list quiet", "alarm list", "Quiet")
    act("list bare", "alarm list /b", "2030-06-01 07:15")
    act("list bare id", "alarm list /b", "|1|")

    # status / cal
    act("status next", "alarm status", "alarm.next")
    act("cal next", "cal next", "Standup")
    act("cal month", "cal 2030-06", "cal.events")

    # enable / disable
    act("disable 1", "alarm disable 1", "disabled")
    act("enable 1", "alarm enable 1", "enabled")

    # soft delete + purge
    act("del 2", "alarm del 2", "soft-deleted")
    act("del all", "alarm del all", "deleted")
    act("purge", "alarm purge", "purged")
    act("status after", "alarm status", "alarm.count")

    # --- fire test: a due alarm (in the past) is fired by the background
    # checker on its next poll (boot catch-up already ran).
    act("add due", "alarm add 1970-01-01 00:00:01 DueNow /beep", "scheduled")
    print("      waiting for the checker to fire it (up to 40s)...")
    wait(ser, "fired", 40)
    b = act("list after fire", "alarm list", "DueNow")
    results.append(("fire-marks-fired", "fired" in b))
    if "fired" in b:
        print("      checker fired the due event (marked fired)")

    # cleanup
    act("del all final", "alarm del all", "deleted")
    act("purge final", "alarm purge", "purged")

    ser.close()
    print("\n===== RESULTS =====")
    fails = [n for n, o in results if not o]
    for n in fails:
        print("  FAIL: %s" % n)
    print("ALARM", "PASS (%d/%d)" % (len(results) - len(fails), len(results)) if not fails else "FAIL")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
