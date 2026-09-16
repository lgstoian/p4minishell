#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tcpterm_test.py - hardware driver for the tcpterm TCP terminal.

Probes the device's own loopback and error paths without needing a host
server. If the on-board httpd is running, the loopback probe must return
HTTP 401; if Wi-Fi/httpd is down, the same probe must cleanly report a
refused connection. Either way the connect/terminate path is exercised.
Prints RESULT OK on success.
Usage: python tools/tcpterm_test.py COMx
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from shell_session import open_port


def run(port):
    ser = open_port(port, timeout=15)
    time.sleep(1)
    ser.reset_input_buffer()

    # Snapshot the server state so the probe expectation matches reality.
    ser.write(b"httpd status\r\n")
    time.sleep(1.0)
    status = re.sub(r"\x1b\[[0-9;]*m", "", ser.read(8192).decode("utf-8", "replace"))
    httpd_up = "state: running" in status

    lines = [
        "tcpterm 127.0.0.1 80 /t:10 GET / HTTP/1.0\\r\\n\\r\\n",
        "tcpterm 127.0.0.1 81 /t:3",
        "tcpterm example.com 99999",
    ]
    for line in lines:
        ser.write((line + "\r\n").encode())
        time.sleep(1.0)

    deadline = time.time() + 60
    out = ""
    while time.time() < deadline:
        chunk = ser.read(65536).decode("utf-8", "replace")
        out += chunk
        if "Usage:" in out and ("refused" in out or "timed out" in out):
            break
        if len(chunk) == 0:
            time.sleep(0.5)
    out = re.sub(r"\x1b\[[0-9;]*m", "", out)

    if httpd_up:
        assert "401 Unauthorized" in out, "loopback HTTP probe failed:\n" + out
        assert re.search(r"closed, \d+ byte\(s\) in 18 out", out), "byte count failed:\n" + out
    else:
        # No listener: the connect must be attempted and cleanly reported.
        assert re.search(r"127\.0\.0\.1:80 .*(refused|timed out)", out), \
            "offline probe failed:\n" + out
    assert "Usage:" in out, "usage path failed:\n" + out
    print("RESULT OK")


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "COM3")
