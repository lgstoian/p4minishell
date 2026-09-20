# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Minimal USB-Serial-JTAG session driver for P4MiniShell bring-up.

Port selection (first match wins): explicit Shell(port=...), the P4_PORT
environment variable, a trailing COMx argv token, else COM11.
"""
import os
import re
import serial
import subprocess
import sys
import time

PROMPT_RE = re.compile(rb"PS .*>\s?$", re.M)
PANICS = [b"Guru Meditation", b"Stack protection", b"Backtrace",
          b"assert failed", b"abort()"]


def default_port():
    """Resolve the serial port for the active board.

    Resolution order (first match wins):
      1. an explicit trailing ``COMx`` argv token;
      2. ``P4_PORT`` (explicit override for a single board);
      3. ``P4_BOARD=<slug>`` resolved through ``tools/board_ports.py`` so two
         boards can be driven at once by exporting a different slug per shell;
      4. ``COM11`` (the historical default).
    """
    for arg in sys.argv[1:]:
        if arg.upper().startswith("COM"):
            return arg.upper()
    explicit = os.environ.get("P4_PORT")
    if explicit:
        return explicit
    board = os.environ.get("P4_BOARD")
    if board:
        try:
            import board_ports
            port = board_ports.resolve_port(board)
            if port:
                return port
        except Exception:
            pass
    return "COM11"


def open_port(port=None, baud=115200, timeout=1):
    """Open the USB-Serial/JTAG port WITHOUT rebooting the board.

    pyserial asserts DTR when a port is opened and the P4 resets on that
    transition. Dropping DTR *after* open() is too late — the reset has already
    fired. Pre-setting the line state on an unopened Serial object makes the
    OS open the port with DTR/RTS low, so the board keeps running.
    """
    ser = serial.Serial()
    ser.port = port or default_port()
    ser.baudrate = baud
    ser.timeout = timeout
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    ser.open()
    time.sleep(0.2)
    return ser


def hard_reset(port=None, baud=460800, chip="esp32p4"):
    """Reboot the board via esptool.

    open_port() deliberately does NOT reset (it would otherwise reboot on
    every host session), so callers that need a fresh boot - unit_run,
    boot_regression - use this. The port must not be held open by the caller.
    """
    cmd = [sys.executable, "-m", "esptool", "--chip", chip,
           "-p", port or default_port(), "-b", str(baud),
           "--before", "default_reset", "--after", "hard_reset", "read_mac"]
    try:
        return subprocess.run(cmd, capture_output=True, timeout=90).returncode == 0
    except Exception:
        return False


class Shell:
    def __init__(self, port=None, baud=115200):
        self.s = open_port(port, baud, timeout=10)
        self.s.reset_input_buffer()

    def run(self, cmd, timeout=15, settle=0.6):
        self.s.reset_input_buffer()
        self.s.write((cmd + "\r\n").encode())
        time.sleep(settle)
        out = b""
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                out += chunk
                for p in PANICS:
                    if p in out:
                        raise RuntimeError("PANIC marker %r after %r" % (p, cmd))
                if PROMPT_RE.search(out[-200:]):
                    break
            else:
                if PROMPT_RE.search(out[-200:]):
                    break
        ansi = re.compile(rb"\x1b\[[0-9;]*m")
        return ansi.sub(b"", out).decode("utf-8", errors="replace")

    def close(self):
        self.s.close()
