#!/usr/bin/env python3
"""db_test.py - comprehensive on-board verification of the `db` command.

Runs a single clean sequence through every db verb and asserts the results.
Usage: python db_test.py [COMx]
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
    """Wait until the shell responds with a settled prompt (not mid-boot).

    Requires the worker's own `ready` output line, not the `echo ready`
    input echo (which the console task prints even while wedged)."""
    stable = 0
    for _ in range(40):
        ser.write(b"echo ready\n")
        b = read_all(ser, 3.0)
        if worker_line(b, "ready") and "PS " in b and "> " in b:
            stable += 1
            if stable >= 2:
                return True
        else:
            stable = 0
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

    # Drain residual boot output until the shell is truly silent, so the very
    # first command's output is never interleaved with or lost behind it.
    silent = 0
    for _ in range(30):
        b = read_all(ser, 1.0)
        if not b:
            silent += 1
            if silent >= 2:
                break
        else:
            silent = 0
    time.sleep(0.5)
    read_all(ser, 1.0)

    # Boot background work (Wi-Fi/SDIO bring-up) contends the shared SDMMC
    # bus for ~15s after the shell first answers; early SD verbs would time
    # out spuriously. Let it quiesce before the first measured command.
    time.sleep(15.0)
    read_all(ser, 1.0)

    results = []
    tag = [0]

    def act(name, cmdline, expect, cap=60):
        # Barrier-sync: an `echo DONE-n` queued right behind the command runs
        # after it on the single worker task (FIFO), so the DONE marker frames
        # this command's output no matter how slow the SD card is. Fixed
        # windows and silence/idle heuristics both misattribute output when
        # one command stalls (every later check then fails on shifted text).
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
                # submit-side `echo DONE-n` input echo: the echo is printed
                # by the console task before the worker runs anything, so
                # breaking on it truncates slow commands and misattributes
                # their output to the next window.
                if worker_line(buf.decode(errors="replace"), marker):
                    break
            else:
                time.sleep(0.05)
        b = ANSI.sub("", buf.decode(errors="replace"))
        ok = (expect in b) and worker_line(b, marker)
        results.append((name, ok))
        print("%-44s %s" % (cmdline, "PASS" if ok else "FAIL(%s)" % expect))
        if not ok:
            print("     ", repr(b[-160:]))
        return b

    # clean slate
    act("drop old", "db drop contacts", "dropped")
    act("drop old2", "db drop t", "not found")
    ser.write(b"del /p DBS/t.EXPORT\n")
    read_all(ser, 1.0)
    ser.write(b"del /p DBS/contacts.EXPORT\n")
    read_all(ser, 1.0)

    # create
    act("create", "db create contacts /cr:APP /tp:CNTC /vr:7", "created")
    act("create dup", "db create contacts", "already exists")
    act("list", "db list", "contacts")
    act("info", "db info contacts", "db.records")

    # add
    act("add alice", "db add contacts /cat:1 /key:alice Alice Smith", "db.added")
    act("add bob", "db add contacts /cat:1 /key:bob Bob Jones", "db.added")
    act("add secret", "db add contacts /cat:2 /secret p4ssw0rd", "db.added")

    # get (incl. secret redaction)
    act("get alice", "db get contacts 1", "Alice Smith")
    act("get bob", "db get contacts 2", "Bob Jones")
    act("get secret redacted", "db get contacts 3", "secret")
    act("get secret revealed", "db get contacts 3 /reveal", "p4ssw0rd")

    # find / count / categories
    act("find by key", "db find contacts /key:bob", "db.matches")
    act("find /b", "db find contacts /b", "1|1|alice")
    act("count", "db count contacts", "db.count")
    act("cats list", "db categories contacts list", "Business")
    act("cats set", "db categories contacts set 7 Family", "category 7")
    act("cats verify", "db categories contacts list", "Family")

    # set
    act("set bob", "db set contacts 2 /key:robert Robert Brown", "updated")
    act("get robert", "db get contacts 2", "Robert Brown")

    # soft delete + purge
    act("soft del", "db del contacts 2", "soft-deleted")
    act("count after del", "db count contacts", "db.count")
    act("purge", "db purge contacts", "purged")
    act("count after purge", "db count contacts", "db.count")

    # export / import round trip into a fresh db
    act("export", "db export contacts", "exported")
    act("drop for import", "db drop contacts", "dropped")
    act("create for import", "db create contacts", "created")
    act("import", "db import contacts", "imported")
    act("get imported alice", "db get contacts 1", "Alice Smith")
    act("get imported secret", "db get contacts 2 /reveal", "p4ssw0rd")
    act("info imported", "db info contacts", "db.records")

    # current-db short forms
    act("open", "db open contacts", "current = contacts")
    act("current get", "db get 1", "Alice Smith")
    act("current count", "db count", "db.count")
    act("current find", "db find /key:robert", "db.matches")
    act("close", "db close", "closed")

    # errorlevel: not found
    act("get missing", "db get contacts 999", "not found")

    ser.close()

    print("\n===== RESULTS =====")
    fails = [n for n, o in results if not o]
    allok = not fails
    for n in fails:
        print("  FAIL: %s" % n)
    print("DB", "PASS (%d/%d)" % (len(results) - len(fails), len(results)) if allok else "FAIL")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
