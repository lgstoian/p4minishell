# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""DeviceSession - the one serial session used by every P4MiniShell test.

Built on :mod:`shell_session` (the DTR-safe open) and :mod:`bg_run` (the
marker-sync idea). Prompt-tail matching alone is unsafe: deferred transcript
output legally trails the prompt, so idle commands match but a busy command
does not. Instead a unique ``echo`` marker is queued behind the command; the
worker is serial, so seeing the marker's own line proves OUR command finished.
Trailing async output is then drained to silence.

Also provides byte-exact binary framing (``BMPX`` screenshots, ``SDFX``
transfers) with a pushback buffer, because a chunked reader otherwise eats
the first bytes of the frame while looking for the "streaming" prelude.
"""
from __future__ import annotations

import os
import re
import sys
import time
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import serial  # noqa: E402

from shell_session import default_port, hard_reset, open_port  # noqa: E402

ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")
ANSI_STR_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
PROMPT_GLUED = re.compile(r"^(?:PS \S*> *)+")
PANICS = (
    b"Guru Meditation",
    b"Stack protection",
    b"Backtrace",
    b"assert failed",
    b"abort()",
    b"Task watchdog",
)

__all__ = ["DeviceSession", "P4Error", "PanicError"]


class P4Error(RuntimeError):
    """Generic device/test failure (timeouts, missing markers, bad protocol)."""


class PanicError(P4Error):
    """A panic/assert marker appeared in the device output."""


def strip_ansi_bytes(data: bytes) -> bytes:
    return ANSI_RE.sub(b"", data)


def strip_ansi(text: str) -> str:
    return ANSI_STR_RE.sub("", text)


class DeviceSession:
    """A live, DTR-safe USB-Serial-JTAG session to the board.

    Use as a context manager so the port is always released::

        with DeviceSession("COM3") as dev:
            print(dev.run("about"))
    """

    def __init__(self, port: Optional[str] = None, baud: int = 115200):
        self.port = (port or default_port()).upper()
        self.baud = baud
        self.ser: Optional[serial.Serial] = None
        self._rx = bytearray()
        self._tag = 0
        self.open()

    # -- lifecycle -------------------------------------------------------
    def open(self) -> None:
        if self.ser is not None:
            return
        self.ser = open_port(self.port, self.baud, timeout=0.3)
        self._rx.clear()

    def close(self) -> None:
        if self.ser is not None:
            try:
                self.ser.close()
            finally:
                self.ser = None

    def __enter__(self) -> "DeviceSession":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def hard_reset(self, settle: float = 0.5) -> None:
        """Power-cycle via esptool, then reopen and wait for the prompt."""
        self.close()
        hard_reset(self.port)
        time.sleep(settle)
        self.open()
        self.wait_prompt(timeout=25.0)

    # -- low-level byte I/O ---------------------------------------------
    def reset_input(self) -> None:
        self._rx.clear()
        if self.ser is not None:
            self.ser.reset_input_buffer()

    def _read_serial(self, n: int, timeout: float) -> bytes:
        assert self.ser is not None
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.ser.read(n)
            if chunk:
                return chunk
        return b""

    def read_exact(self, n: int, timeout: float = 30.0) -> bytes:
        """Read exactly ``n`` bytes, using the pushback buffer first."""
        out = bytearray()
        if self._rx:
            take = min(n, len(self._rx))
            out += self._rx[:take]
            del self._rx[:take]
        end = time.time() + timeout
        while len(out) < n and time.time() < end:
            chunk = self._read_serial(n - len(out), max(0.1, end - time.time()))
            if not chunk:
                continue
            out += chunk
        if len(out) != n:
            raise P4Error("read_exact(%d) got %d bytes" % (n, len(out)))
        return bytes(out)

    def read_until(self, marker: bytes, timeout: float = 15.0) -> bytes:
        """Read through ``marker`` inclusive; leftover goes to the pushback."""
        marker = marker.encode() if isinstance(marker, str) else marker
        buf = bytearray(self._rx)
        self._rx.clear()
        end = time.time() + timeout
        while marker not in buf and time.time() < end:
            chunk = self._read_serial(4096, max(0.1, end - time.time()))
            if chunk:
                buf += chunk
        idx = buf.find(marker)
        if idx < 0:
            self._rx.extend(buf)
            return bytes(buf)
        end_i = idx + len(marker)
        self._rx.extend(buf[end_i:])
        return bytes(buf[:end_i])

    def read_for(self, seconds: float) -> str:
        """Drain whatever arrives in the window; returns ANSI-stripped text."""
        buf = bytearray(self._rx)
        self._rx.clear()
        end = time.time() + seconds
        while time.time() < end:
            chunk = self._read_serial(4096, max(0.05, end - time.time()))
            if chunk:
                buf += chunk
        text = strip_ansi(strip_ansi_bytes(bytes(buf)).decode("utf-8", "replace"))
        self._check_panic(text)
        return text

    def wait_prompt(self, timeout: float = 15.0) -> str:
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self._read_serial(4096, max(0.05, end - time.time()))
            if not chunk:
                continue
            buf += chunk
            text = strip_ansi(strip_ansi_bytes(bytes(buf)).decode("utf-8", "replace"))
            if text.rstrip().endswith(">") or re.search(r"PS .*>\s*$", text):
                break
        text = strip_ansi(strip_ansi_bytes(bytes(buf)).decode("utf-8", "replace"))
        self._check_panic(text)
        return text

    def write(self, data: bytes) -> None:
        assert self.ser is not None
        self.ser.write(data)

    def write_line(self, text: str) -> None:
        self.write((text + "\r\n").encode())

    # -- marker-synchronised command execution ---------------------------
    def _next_tag(self) -> str:
        # No leading/trailing underscores: the shell `echo` treats `__x__` as
        # bold and `_x_` as italic markup, so those delimiters never survive.
        self._tag += 1
        return "P4TAG%dX%d" % (int(time.time()) % 1000000, self._tag)

    @staticmethod
    def _marked(text: str, tag: str) -> bool:
        for line in text.splitlines():
            raw = strip_ansi(line)
            if ("echo %s" % tag) in raw:
                continue
            if PROMPT_GLUED.sub("", raw).strip() == tag:
                return True
        return False

    @staticmethod
    def _drop_echo(text: str, cmd: str) -> str:
        """Remove the shell's echo of ``cmd`` from ``text``.

        The interactive prompt echoes every submitted line, so a captured
        transcript contains the command itself. A negative assertion such as
        ``if exist X echo FLAG`` (want=False) would then match the echoed
        command and always fail. Dropping the one line equal to the submitted
        command makes the returned text the command's *response* only.
        """
        target = strip_ansi(cmd).strip()
        out = []
        removed = False
        for line in text.splitlines():
            probe = PROMPT_GLUED.sub("", strip_ansi(line)).strip()
            if not removed and target and probe == target:
                removed = True
                continue
            out.append(line)
        return "\n".join(out)

    def run(
        self,
        cmd: str,
        timeout: float = 30.0,
        settle: float = 0.5,
        silence: float = 1.2,
        drop_echo: bool = True,
    ) -> str:
        """Run ``cmd`` and return its ANSI-stripped output.

        Queues ``echo <unique>`` behind the command and drains until that
        marker line appears, then keeps reads until ``silence`` seconds of no
        traffic (deferred transcript output, background chatter). The echo of
        ``cmd`` is removed unless ``drop_echo`` is false.
        """
        assert self.ser is not None
        tag = self._next_tag()
        self.reset_input()
        self.write_line(cmd)
        time.sleep(settle)
        self.write_line("echo " + tag)

        buf = bytearray()
        end = time.time() + timeout
        marked = False
        last_data = time.time()
        while time.time() < end:
            chunk = self._read_serial(4096, 0.3)
            if chunk:
                buf += chunk
                last_data = time.time()
                text = strip_ansi(strip_ansi_bytes(bytes(buf)).decode("utf-8", "replace"))
                self._check_panic(text)
                if self._marked(text, tag):
                    marked = True
            else:
                if marked and time.time() - last_data > silence:
                    break
        text = strip_ansi(strip_ansi_bytes(bytes(buf)).decode("utf-8", "replace"))
        if not marked:
            self._check_panic(text)
            raise P4Error("run(%r): marker %s not seen within %ss\n%s"
                          % (cmd, tag, timeout, text[-800:]))
        if drop_echo:
            text = self._drop_echo(text, cmd)
        return text

    # -- interactive helpers --------------------------------------------
    def send(self, text: str, settle: float = 0.4) -> str:
        """Send a line without waiting for a marker; return what follows."""
        self.write_line(text)
        return self.read_for(settle)

    def read_binary_frame(self, magic: bytes, size_timeout: float = 15.0,
                          data_timeout: float = 60.0) -> bytes:
        # Scan through any interleaved log text (the board keeps emitting
        # E/W/I lines while a binary stream is framed) until the magic appears,
        # instead of assuming the very next bytes are the header. Without this,
        # a single log line between the "streaming" notice and the frame made
        # the capture fail with `bad frame magic` (the visual-sweep flake).
        prelude = self.read_until(magic, size_timeout)
        if magic not in prelude:
            raise P4Error("no frame magic %r in stream (%r)"
                          % (magic, bytes(prelude[-120:])))
        size = int.from_bytes(self.read_exact(4, size_timeout), "little")
        return self.read_exact(size, data_timeout)

    # -- panic detection -------------------------------------------------
    @staticmethod
    def _check_panic(text: str) -> None:
        for marker in PANICS:
            if marker.decode() in text:
                raise PanicError("panic marker %r in output" % marker.decode())
