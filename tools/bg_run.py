#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""bg_run.py - shared marker-sync driver for bg lifecycle tests.

Prompt-tail matching cannot work here: deferred transcript output legally
trails the prompt (`PS /sdcard> PS /sdcard> HI`), so `$`-anchored prompt
regexes only match idle commands. run_quiet() sends the command plus a
marker echo; the worker is serial, so the marker's output line proves OUR
command completed. Then it drains trailing async output until silence.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "apps", "companion"))
import deep_test as D

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import Shell, PANICS

ANSI = re.compile(rb"\x1b\[[0-9;]*m")
ANSI_STR = re.compile(r"\x1b\[[0-9;]*m")
PROMPT_GLUED = re.compile(r"^(PS \S*> *)+")


def marker_done(text, tag):
    """Like D.worker_line, but strips ALL glued `PS ...>` prefixes: deferred
    output legally doubles the prompt (`PS /sdcard> PS /sdcard> TAG`)."""
    for ln in text.splitlines():
        s = ANSI_STR.sub("", ln).strip()
        s = PROMPT_GLUED.sub("", s).strip()
        if s == tag and ("echo %s" % tag) not in ANSI_STR.sub("", ln):
            return True
    return False


def run_quiet(sh, cmd, timeout=30, silence=1.5, tag=None):
    tag = tag or ("T%d" % (int(time.time() * 10) % 100000))
    sh.s.timeout = 0.3  # Shell() sets 10 s; idle drains must return fast
    sh.s.reset_input_buffer()
    sh.s.write((cmd + "\r\n").encode())
    time.sleep(0.6)
    sh.s.write(("echo %s\r\n" % tag).encode())
    out = b""
    end = time.time() + timeout
    last_data = time.time()
    marked = False
    while time.time() < end:
        chunk = sh.s.read(4096)
        if chunk:
            out += chunk
            last_data = time.time()
            for p in PANICS:
                if p in out:
                    raise RuntimeError("PANIC marker %r after %r" % (p, cmd))
            text = ANSI.sub(b"", out).decode("utf-8", errors="replace")
            if marker_done(text, tag):
                marked = True
        else:
            if marked and time.time() - last_data > silence:
                break
    return ANSI.sub(b"", out).decode("utf-8", errors="replace")


def boot(port="COM3"):
    sh = Shell(port)
    time.sleep(12)
    sh.s.reset_input_buffer()
    run_quiet(sh, "delay 12000")  # let boot wifi chatter finish
    return sh
