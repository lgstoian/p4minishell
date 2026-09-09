"""Stall catcher for bugs.md O3 (transient worker stalls + queue-full storms).

Runs mixed SD / non-SD probes with output-anchored timing until a stall is
caught or the budget expires. A stall is significant only if non-SD probes
stay fast while SD probes hang (SD-path stall) or everything hangs (global
worker stall). Queue-full lines and mount state are recorded throughout.

Usage: python stall_catch.py [COMx] [minutes]
"""

import re
import serial
import sys
import time
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

PORT = sys.argv[1] if len(sys.argv) > 1 else default_port()
BUDGET_MIN = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

# (command, matcher-lambda-or-substring, touches-SD)
PROBES = [
    ("echo STALLCATCH", "STALLCATCH", False),
    ("db list", "db.databases", True),
    ("calc 6*7", "==42", False),
    ("sd info", "fs_total", True),
]

ANSI = re.compile(rb"\x1b\[[0-9;]*m")
PROMPT_PREFIX = re.compile(r"^PS \S*> ")


def run_once(ser, cmd, expect):
    """Returns (t_echo_s, t_out_s, ok, saw_queue_full). Output-anchored: the
    expect text must arrive as command output (not the input echo).

    t_echo measures the submit path (console task + LVGL port lock for the
    input echo); t_out - t_echo measures the worker. Echo-slow/output-slow
    implicates submit/PORT/LVGL; echo-fast/output-slow implicates the worker
    (or whatever the worker waits on after dequeue)."""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    t0 = time.time()
    buf = b""
    qfull = False
    t_echo = None
    while time.time() - t0 < 60:
        d = ser.read(ser.in_waiting or 1)
        if d:
            buf += d
            if b"queue full" in buf:
                qfull = True
            txt = ANSI.sub(b"", buf).decode(errors="replace")
            for ln in txt.splitlines():
                s = ln.strip()
                if t_echo is None and s.startswith("PS") and cmd in s:
                    t_echo = time.time() - t0
                # Tolerate prompt-glue (normal under load: the previous
                # command's trailing prompt has no newline, so it shares a
                # line with this output). Strip one prompt prefix, then skip
                # only the input-echo line itself (it contains the cmd).
                s = PROMPT_PREFIX.sub("", s).strip()
                if cmd in s:
                    continue  # input echo, not output
                if expect.startswith("=="):
                    if s == expect[2:]:
                        return t_echo, time.time() - t0, True, qfull
                elif expect in s:
                    return t_echo, time.time() - t0, True, qfull
        else:
            time.sleep(0.05)
    tail = ANSI.sub(b"", buf)[-500:].decode(errors="replace")
    print("CAPTURE %s tail=%r" % (cmd, tail), flush=True)
    return t_echo, time.time() - t0, False, qfull


def main():
    ser = open_port(PORT, 115200, timeout=1)
    time.sleep(15.0)  # boot quiesce
    ser.reset_input_buffer()
    t_end = time.time() + BUDGET_MIN * 60
    slow = []
    round_no = 0
    while time.time() < t_end:
        round_no += 1
        for cmd, expect, uses_sd in PROBES:
            t_echo, dt, ok, qfull = run_once(ser, cmd, expect)
            flag = ""
            if not ok or dt > 8.0:
                flag = "  <-- SLOW" if ok else "  <-- TIMEOUT"
                slow.append((round_no, cmd, t_echo, dt, ok, uses_sd, qfull))
            echo_s = ("%.1f" % t_echo) if t_echo is not None else "none"
            print("%.1fs(out)/%s(echo) %-12s sd=%s ok=%s%s"
                  % (dt, echo_s, cmd, uses_sd, ok, flag), flush=True)
            if qfull:
                print("!!! queue-full observed during %s" % cmd, flush=True)
    ser.close()
    print("\n--- slow/timeout events: %d ---" % len(slow))
    for r, c, t_echo, dt, ok, uses_sd, qfull in slow:
        echo_s = ("%.1f" % t_echo) if t_echo is not None else "none"
        print("round %d %-12s out=%.1fs echo=%s ok=%s sd=%s qfull=%s"
              % (r, c, dt, echo_s, ok, uses_sd, qfull))
    # Verdict sketch: SD-only slowness implicates the SD path (mount/bus
    # contention, N1 family); global slowness implicates the worker/LVGL.
    sd_slow = sum(1 for _, _, _, _, ok, uses_sd, _ in slow if uses_sd and not ok)
    glob_slow = sum(1 for _, _, _, _, ok, uses_sd, _ in slow if not uses_sd and not ok)
    print("sd-stalled: %d, global-stalled: %d" % (sd_slow, glob_slow))


if __name__ == "__main__":
    main()
