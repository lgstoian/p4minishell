#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Board-free exercise of the `receive` reply parser in `sdbridge.py`.

`push_file()` is ACK-paced: the device reports the *cumulative* byte count it
accepted for every chunk, and a successful exchange ends with DONE or with the
transcript's `receive: wrote <n> bytes` line. A real board is not needed to
prove the parser and the sender agree; a fake device replaces the serial port.

Run: `python tools/p4test/sdbridge_selftest.py` (exits non-zero on failure).

The 2.5 MB case is the regression this exists for: above ~1 MB the transfer
spends long enough in ACK/footer traffic that the old 15 s footer window could
report a false failure, and writing the next chunk before the device's count
caught up could overrun its USB ring.
"""
from __future__ import annotations

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from p4test import sdbridge  # noqa: E402
from p4test.session import P4Error  # noqa: E402


class FakeDevice:
    """Minimal `DeviceSession` surface for one `receive` exchange.

    Writes are interpreted as the command, one data chunk at a time, then the
    CRC trailer - exactly the firmware's protocol. Replies can be split across
    `read_until` calls, merged with log text, or end with either footer form.
    """

    def __init__(self, size: int, *, partial_every: int = 0,
                 split_replies: int = 0, footer: str = "both",
                 crc_ok: bool = True):
        self.size = size
        self.partial_every = partial_every
        self.split_replies = split_replies
        self.footer = footer
        self.crc_ok = crc_ok
        self.rx = bytearray()
        self.pending = bytearray()
        self.max_pending = 0
        self.received = bytearray()
        self.acked = 0
        self.command = b""
        self.chunks = 0
        self.crc = None
        self.finished = False

    # -- DeviceSession surface -------------------------------------------
    def reset_input(self) -> None:
        self.rx.clear()

    def read_for(self, _seconds: float) -> str:
        return ""

    def read_until(self, marker, timeout: float = 1.0) -> bytes:
        marker = marker.encode() if isinstance(marker, str) else marker
        if marker == sdbridge.READY:
            idx = self.rx.find(sdbridge.READY)
            if idx < 0:
                return b""
            out = bytes(self.rx[:idx + len(sdbridge.READY)])
            del self.rx[:idx + len(sdbridge.READY)]
            return out
        assert marker == b"\n"
        idx = self.rx.find(b"\n")
        if idx < 0 and self.split_replies > 0 and self.rx:
            # A USB read ended mid-line: hand back a fragment without a
            # newline. The real DeviceSession pushes those bytes back, so a
            # retry reads the whole line; keep them here to match.
            self.split_replies -= 1
            take = max(1, len(self.rx) // 2)
            return bytes(self.rx[:take])
        if idx < 0:
            out = bytes(self.rx)
            self.rx.clear()
            return out
        out = bytes(self.rx[:idx + 1])
        del self.rx[:idx + 1]
        return out

    def _pump(self) -> None:
        """Consume queued host bytes the way the receive pump does.

        A read can stop short of the chunk it was asked for (the regression
        model); the rest stays queued and is consumed by the following read,
        which emits another ACK. That second ACK is what the host waits for
        before writing again.
        """
        while self.pending and self.acked < self.size:
            self.chunks += 1
            want = min(len(self.pending), sdbridge.CHUNK,
                       self.size - self.acked)
            got = want
            if (self.partial_every and self.chunks % self.partial_every == 0
                    and got > 100):
                got -= 100
            self.received += self.pending[:got]
            del self.pending[:got]
            self.acked += got
            if self.chunks % 7 == 0:
                # Interleaved console/log text before the ACK.
                self.rx += b"wifi: scan done\r\n"
            self.rx += b"RX %d\r\n" % self.acked

    def write(self, data: bytes) -> None:
        if not self.command:
            # Command line; a real device answers with READY and the prompt
            # newline.
            self.command = data
            self.rx += b"\n" + sdbridge.READY + b"\n"
            return
        if self.finished:
            return
        if self.acked < self.size:
            self.pending += data
            if len(self.pending) > self.max_pending:
                self.max_pending = len(self.pending)
            self._pump()
            return

        # CRC trailer (4 bytes, little-endian).
        self.crc = struct.unpack("<I", data)[0]
        self.finished = True
        expected = zlib.crc32(bytes(self.received)) & 0xFFFFFFFF
        if not self.crc_ok or self.crc != expected:
            self.rx += (b"\x1b[31mreceive: transfer failed - CRC mismatch "
                        b"or missing trailer\x1b[0m\r\n")
            return
        if self.footer in ("both", "done"):
            self.rx += sdbridge.DONE + b"\n"
        if self.footer in ("both", "wrote"):
            self.rx += (b"\x1b[32mreceive: wrote %d bytes to %s in 1 ms"
                        b"\x1b[0m\r\n" % (self.size, self.command.split()[1]))


def _data(size: int) -> bytes:
    # Deterministic but not compressible.
    return bytes((i * 31 + (i >> 8) * 17) & 0xFF for i in range(size))


def _expect_ok(size: int, **kwargs) -> None:
    dev = FakeDevice(size, **kwargs)
    data = _data(size)
    sdbridge.push_file(dev, "BIG.EPUB", data, timeout=5.0, retries=1)
    assert bytes(dev.received) == data, "assembled bytes differ from source"
    assert dev.acked == size, "device accepted %d/%d" % (dev.acked, size)
    assert dev.crc == zlib.crc32(data) & 0xFFFFFFFF, "CRC trailer wrong"
    assert dev.max_pending <= sdbridge.CHUNK, \
        "host queued %d bytes (> one chunk): ACK pacing is broken" % dev.max_pending


def _expect_fail(size: int, fragment: bytes, **kwargs) -> None:
    dev = FakeDevice(size, **kwargs)
    data = _data(size)
    try:
        sdbridge.push_file(dev, "BIG.EPUB", data, timeout=2.0, retries=1)
    except P4Error as exc:
        assert fragment in str(exc).encode(), \
            "failure %r does not mention %r" % (str(exc), fragment)
        return
    raise AssertionError("push_file reported success for a failed exchange")


def main() -> int:
    # An empty file has no data ACK: the host must go straight to the CRC
    # trailer instead of waiting for an ACK that will never come.
    _expect_ok(0)

    # Above 1 MB: 640 chunks, every 5th accepted partially, replies split in
    # half now and then, and log text interleaved with the ACKs.
    _expect_ok(2_500_000, partial_every=5, split_replies=6)

    # DONE arriving on its own still proves the exchange.
    _expect_ok(200_000, footer="done")

    # A slow/busy console can drop the DONE window; the device's own "wrote"
    # line is the second proof of success.
    _expect_ok(1_100_000, footer="wrote")

    # A real CRC failure must still be reported, with the device's reason.
    _expect_fail(200_000, b"CRC mismatch", crc_ok=False)

    # Truncated exchange (footer moved out of the window) is a failure too.
    _expect_fail(200_000, b"no DONE", footer="none")

    print("sdbridge selftest: PASS (ACK pacing, split replies, 2.5 MB, CRC, fail)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
