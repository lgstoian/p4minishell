#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""timer_test.py - hardware driver for the timer/stopwatch verb.

Exercises start/lap/stop/status, /v:NAME storage, and the usage/error
paths over the serial console. Prints RESULT OK on success.
Usage: python tools/timer_test.py COMx

Note: command lines are burst-written up front and the output drained
afterwards. Reading between commands would stall the host (blocking
reads) and inflate the board-side elapsed time under test.
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from shell_session import open_port


def run(port):
    ser = open_port(port, timeout=10)
    time.sleep(1)
    ser.reset_input_buffer()

    lines = [
        "timer start ttest",
        "delay 1200",
        "timer lap ttest /v:LAPMS",
        "timer stop ttest /v:STOPMS",
        "echo LAPMS=%LAPMS%",
        "timer status",
        "timer stop ttest",
        "timer frobnicate",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(0.4)

    deadline = time.time() + 30
    out = ""
    while time.time() < deadline:
        chunk = ser.read(32768).decode("utf-8", "replace")
        out += chunk
        if "Usage:" in out and out.count("timer stop ttest") >= 2:
            break
        if len(chunk) == 0:
            time.sleep(0.5)

    assert "started" in out, "start failed:\n" + out
    m = re.search(r"lap at (\d+) ms", out)
    assert m, "lap failed:\n" + out
    lap = int(m.group(1))
    assert 1100 <= lap <= 5000, f"lap out of range: {lap}"
    m = re.search(r"stopped at (\d+) ms", out)
    assert m, "stop failed:\n" + out
    stop = int(m.group(1))
    assert stop >= lap, f"stop {stop} < lap {lap}"
    assert re.search(r"LAPMS=" + str(lap), out.replace(" ", "")), "var store failed:\n" + out
    assert "ttest" in out and "stopped" in out, "status failed:\n" + out
    assert "no running run" in out, "double-stop should fail:\n" + out
    assert "Usage:" in out, "bad verb should print usage:\n" + out
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
