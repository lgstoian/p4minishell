"""Minimal USB-Serial-JTAG session driver for P4MiniShell bring-up."""
import re
import serial
import time

PROMPT_RE = re.compile(rb"PS .*>\s?$", re.M)
PANICS = [b"Guru Meditation", b"Stack protection", b"Backtrace",
          b"assert failed", b"abort()"]


class Shell:
    def __init__(self, port="COM11", baud=115200):
        self.s = serial.Serial(port, baud, timeout=10)
        self.s.dtr = False
        self.s.rts = False
        time.sleep(0.5)
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
