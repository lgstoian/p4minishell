#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""transcript_perf.py - per-command transcript cost bench (bugs.md F26).

Times interactive `echo` commands at three payload lengths against two
session states: a fresh `cls` (short scrollback) and a filled scrollback
(default 450 x 120-char lines, past the 384-span render cap). The serial
worker is serialized against the LVGL apply (both take the port lock), so
the measured command-to-output latency tracks the per-repaint cost the F25
campaign recorded (echo 100/250/500 -> 374/531/881 ms on the Tab5).

Usage: python tools/transcript_perf.py [COMx] [--fill N] [--runs R]
"""

import os
import re
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port, hard_reset

ANSI = re.compile(r"\x1b\[[0-9;]*m")
ANSI_B = re.compile(rb"\x1b\[[0-9;]*m")
PROMPT_GLUED = re.compile(r"^(PS \S*> *)+")
PROMPT_ANY = re.compile(r"^PS .*?> *")

LOGF = None


def lograw(text):
    if LOGF is not None:
        LOGF.write(text)
        LOGF.flush()

LEN_LIST = (100, 250, 500)
RUNS = 5
FILL_LINES = 450


def strip_line(ln):
    s = ANSI.sub("", ln).strip()
    s = PROMPT_GLUED.sub("", s)
    return PROMPT_ANY.sub("", s).strip()


def wait_payload(ser, payload, timeout=8.0, prebuf=None):
    """Read until a transcript line is exactly the payload (the command echo
    line contains 'echo ' and must not match). Line accumulation must survive
    payload lines split across read chunks."""
    buf = prebuf if prebuf is not None else ""
    end = time.time() + timeout
    while time.time() < end:
        n = ser.in_waiting
        chunk = ser.read(n if n else 1)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            lograw(text)
            buf += text
            idx = buf.rfind("\n")
            if idx >= 0:
                whole, buf = buf[:idx], buf[idx + 1:]
            else:
                whole, buf = "", buf
            for ln in whole.splitlines():
                t = strip_line(ln)
                if t == payload and "echo " not in t:
                    return True, buf
    return False, buf


def timed_echo(ser, payload, timeout=8.0):
    ser.reset_input_buffer()
    t0 = time.perf_counter()
    ser.write(("echo %s\r\n" % payload).encode())
    ok, _ = wait_payload(ser, payload, timeout)
    dt = time.perf_counter() - t0
    return dt, ok


def payload_for(tag, width):
    """Single token of exactly `width` chars: Q<tag>.<pad>x, terminated so the
    total length is the requested one."""
    head = "Q%s.P" % tag
    pad = width - len(head)
    if pad < 1:
        pad = 1
    return (head + "x" * pad)[:width]


def fill_scrollback(ser, lines, width=120):
    """Append `lines` echo commands, one in flight at a time, recording the
    per-command latency as the session grows (the F26 cost curve)."""
    print("filling scrollback with %d x %d-char echoes (one in flight)..." % (lines, width), flush=True)
    t0 = time.time()
    buf = ""
    dts = []
    for i in range(lines):
        tag = "F%d" % i
        p = payload_for(tag, width)
        ser.write(("echo %s\r\n" % p).encode())
        s0 = time.perf_counter()
        ok, buf = wait_payload(ser, p, timeout=20.0, prebuf=buf)
        dt = time.perf_counter() - s0
        dts.append(dt * 1000.0)
        if not ok:
            print("  line %d lost (resync)" % i, flush=True)
        if (i + 1) % 100 == 0:
            k = len(dts)
            print("  %d lines: recent medians %.0f/%.0f/%.0f ms (%.0fs)" % (
                i + 1, statistics.median(dts[-30:]),
                statistics.median(dts[max(0, k // 2 - 15):max(0, k // 2 + 15)]),
                statistics.median(dts[:30]), time.time() - t0), flush=True)
    # Drain until quiet so measurement starts on an idle renderer.
    time.sleep(1.5)
    while True:
        ser.reset_input_buffer()
        d = ser.read(65536)
        if not d:
            break
    print("  filled in %.0fs" % (time.time() - t0), flush=True)


def measure(ser, runs, state):
    print("== %s ==" % state, flush=True)
    for width in LEN_LIST:
        samples = []
        for r in range(runs):
            p = payload_for("T%d" % r, width)
            dt, ok = timed_echo(ser, p)
            if not ok:
                print("  L=%-3d run %d: TIMEOUT" % (width, r), flush=True)
                continue
            samples.append(dt * 1000.0)
        if samples:
            print("  L=%-3d median %6.0f ms  min %6.0f  max %6.0f  (n=%d)" % (
                width, statistics.median(samples), min(samples),
                max(samples), len(samples)), flush=True)


def main():
    global LOGF
    port = default_port()
    runs, fill = RUNS, FILL_LINES
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--runs":
            i += 1
            runs = int(args[i])
        elif a == "--fill":
            i += 1
            fill = int(args[i])
        elif a == "--log":
            i += 1
            LOGF = open(args[i], "w", encoding="utf-8")
        elif re.match(r"^COM\d+$", a, re.I):
            port = a
        else:
            print(__doc__)
            return 1
        i += 1

    hard_reset(port)
    ser = open_port(port)
    try:
        ser.timeout = 0.3
        ready = False
        end = time.time() + 60
        attempt = 0
        while time.time() < end and not ready:
            attempt += 1
            p = "READY%d" % attempt
            ser.write(("echo %s\r\n" % p).encode())
            ready, _ = wait_payload(ser, p, timeout=8.0)
        if not ready:
            print("board did not reach a prompt", flush=True)
            return 1

        print("port %s  (fill=%d, runs=%d)" % (port, fill, runs), flush=True)
        timed_echo(ser, payload_for("WARM", 100))
        measure(ser, runs, "fresh boot (short scrollback)")
        fill_scrollback(ser, fill)
        ser.write(b"cls\r\n")
        time.sleep(0.8)
        ser.reset_input_buffer()
        measure(ser, runs, "after cls")
        fill_scrollback(ser, fill)
        measure(ser, runs, "filled scrollback")
    finally:
        ser.close()


main()
