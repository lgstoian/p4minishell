#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tcpterm_test.py - hardware driver for the tcpterm TCP terminal.

Probes the device's own loopback and error paths without needing a host
server. If the on-board httpd is running, the loopback probe must return
HTTP 401; if Wi-Fi is up but no listener is present, the same probe must
cleanly report a refused connection. When Wi-Fi is down (no active
connection) the probe is meaningless and the run is reported as SKIP.
Prints RESULT OK on success.
Usage: python tools/tcpterm_test.py COMx
"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from shell_session import open_port


def _wifi_connected(ser):
    """Best-effort `wifi status` parse; the connected line is the source of
    truth for whether tcpterm has a connection to use."""
    ser.reset_input_buffer()
    ser.write(b"wifi status\r\n")
    end = time.time() + 8.0
    text = ""
    while time.time() < end:
        chunk = ser.read(8192)
        if chunk:
            text += chunk.decode("utf-8", "replace")
            if "connected:" in text:
                break
        else:
            time.sleep(0.2)
    return "connected: yes" in re.sub(r"\x1b\[[0-9;]*m", "", text)


def _wait_wifi_connected(ser, timeout=45.0):
    """Poll for the link instead of skipping on the first look: a fresh boot
    needs a few seconds to associate, and `regression.py` resets the board
    before every step."""
    end = time.time() + timeout
    while time.time() < end:
        if _wifi_connected(ser):
            return True
        time.sleep(3.0)
    return False


def run(port):
    ser = open_port(port, timeout=15)
    time.sleep(1)
    ser.reset_input_buffer()

    # Snapshot the server state so the probe expectation matches reality.
    ser.write(b"httpd status\r\n")
    time.sleep(1.0)
    status = re.sub(r"\x1b\[[0-9;]*m", "", ser.read(8192).decode("utf-8", "replace"))
    httpd_up = "state: running" in status

    # No active connection: tcpterm can only refuse the request, so the
    # loopback/refused expectations below are not meaningful.
    if not _wait_wifi_connected(ser):
        print("RESULT SKIP (Wi-Fi not connected; tcpterm needs an active connection)")
        return

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
