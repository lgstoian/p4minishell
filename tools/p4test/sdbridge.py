# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Byte-exact SD transfers over the serial link.

``receive <path> <size> /crc`` and ``send <path>`` are the firmware's own
binary channels. Both are wrapped here once; the deployment tools and every
suite call these instead of re-implementing ACK pacing or SDFX framing.
"""
from __future__ import annotations

import os
import re
import struct
import time
import zlib
from typing import Optional, Tuple

from .session import DeviceSession, P4Error

CHUNK = 4096
READY = b"=== RX READY ==="
DONE = b"=== RX DONE ==="
# Success line the device prints *after* DONE. Accepting it as a second proof
# matters for a large transfer: the footer and the transcript's success line
# are queued behind ~1 MB of ACKs, and a busy console can deliver the line
# after the reply window that held DONE has expired.
WROTE_RE = re.compile(rb"receive: wrote (\d+) bytes")
ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")
# Error text the receive engine prints on a real failure (the device removes
# the partial destination and never writes DONE). The transfer must still be
# reported as a failure, and the device's own reason is the useful one.
ERROR_WORDS = (
    b"fail", b"incomplete", b"mismatch", b"timed out", b"cannot",
    b"not present", b"out of memory", b"invalid",
)


def _parse_ack(reply: bytes) -> Tuple[Optional[int], bool]:
    """Parse one reply chunk into ``(cumulative, done)``.

    The device answers every accepted chunk with ``RX <cumulative>\\n``. A
    console under load can merge that line with transcript/log text and can
    coalesce several ACKs, so the *last* complete ``RX <n>`` line in the chunk
    wins and non-ACK lines are ignored. ``DONE`` anywhere means the exchange
    ended (the caller decides whether that was expected yet).
    """
    clean = reply.replace(b"\r", b"")
    cumulative = None
    for line in clean.split(b"\n"):
        line = line.strip()
        if not line.startswith(b"RX "):
            continue
        try:
            cumulative = int(line[3:].split()[0])
        except (IndexError, ValueError):
            continue
    return cumulative, DONE in clean


def _reply_error(reply: bytes) -> Optional[str]:
    """The device's ``receive:`` error line from @p reply, if it carries one."""
    clean = ANSI_RE.sub(b"", reply.replace(b"\r", b""))
    for line in clean.split(b"\n"):
        line = line.strip()
        if b"receive:" not in line:
            continue
        lowered = line.lower()
        if any(word in lowered for word in ERROR_WORDS):
            return line.decode("utf-8", "replace")
    return None


def _wrote_size(reply: bytes) -> Optional[int]:
    """Byte count from the device's success line in @p reply, if present."""
    match = WROTE_RE.search(ANSI_RE.sub(b"", reply))
    return int(match.group(1)) if match is not None else None


def _read_reply_line(dev: DeviceSession, timeout: float) -> Optional[bytes]:
    """Read one newline-terminated reply line, or None on timeout.

    ``DeviceSession.read_until`` returns through the first newline; when its
    window expires first it returns the partial bytes *and* pushes them back
    into the session buffer. So this only accepts a complete line and otherwise
    retries: a split ``RX 1...`` must not be parsed as a much smaller count,
    and must not be concatenated with its own pushed-back copy.
    """
    end = time.time() + timeout

    while True:
        remaining = end - time.time()
        if remaining <= 0:
            return None
        chunk = dev.read_until(b"\n", remaining)
        if not chunk:
            return None
        if chunk.endswith(b"\n"):
            return chunk


def push_file(dev: DeviceSession, remote: str, data: bytes,
              timeout: float = 90.0, retries: int = 4) -> None:
    """Upload ``data`` to ``remote`` via the CRC-checked ``receive`` path.

    Retries on a failed/CRC-mismatched exchange: the device removes the
    partial destination, so the next attempt starts clean. Transient
    USB-Serial/JTAG byte loss under load can otherwise fail an in-memory
    push (which has no other recovery), e.g. the `s06_batch` P4BTEST.BAT
    transfer. Between attempts the input is drained and allowed to settle so
    a half-consumed footer cannot desync the next READY marker.
    """
    last = None
    for attempt in range(retries):
        try:
            _push_file_once(dev, remote, data, timeout)
            return
        except P4Error as exc:
            last = exc
            if attempt + 1 >= retries:
                break
            try:
                dev.reset_input()
                dev.read_for(0.4)
            except Exception:  # noqa: BLE001
                pass
            time.sleep(1.5 + attempt)
    raise P4Error("push %s failed after %d retries: %s" % (remote, retries, last))


def _push_file_once(dev: DeviceSession, remote: str, data: bytes,
                    timeout: float) -> None:
    """One `receive` exchange (see `push_file` for the retry contract)."""
    dev.reset_input()
    size = len(data)
    dev.write(("receive %s %d /crc\r\n" % (remote, size)).encode())
    ready = dev.read_until(READY, 10.0)
    if READY not in ready:
        raise P4Error("receive %s: no READY (%r)" % (remote, ready[-120:]))
    dev.read_until(b"\n", 2.0)  # drop the newline after READY

    # ACK pacing: keep ONE chunk in flight. The device's count is how many
    # bytes it has written, and its USB ring can hold a remainder of the last
    # write; writing the next chunk before that remainder is acknowledged
    # either overruns the ring (which drops bytes under a burst) or appends
    # data after a queued tail. So write only when `acked == sent`, and let a
    # partial ACK drain through the replies that follow.
    sent = 0
    acked = 0
    if size > 0:
        nxt = min(CHUNK, size)
        dev.write(data[0:nxt])
        sent = nxt
    stall_deadline = time.time() + timeout
    while acked < size:
        reply = _read_reply_line(dev, max(1.0, stall_deadline - time.time()))
        if reply is None:
            raise P4Error("receive %s: no ACK at %d/%d bytes"
                          % (remote, acked, size))
        cumulative, done = _parse_ack(reply)
        if done:
            # A footer before every byte was acknowledged means the exchange
            # ended early; do not report a truncated file as success.
            raise P4Error("receive %s: DONE at %d/%d bytes"
                          % (remote, acked, size))
        if cumulative is None:
            error = _reply_error(reply)
            if error is not None:
                raise P4Error("receive %s: %s" % (remote, error))
            # Interleaved log text (Wi-Fi chatter etc.) is not an ACK: skip it
            # and keep reading. The stall deadline is only refreshed by an ACK
            # that moves the count, so a wedged device cannot loop forever.
            if time.time() > stall_deadline:
                raise P4Error("receive %s: bad ACK %r" % (remote, reply[-80:]))
            continue
        if cumulative > sent:
            raise P4Error("receive %s: ACK %d beyond %d bytes sent"
                          % (remote, cumulative, sent))
        if cumulative > acked:
            acked = cumulative
            stall_deadline = time.time() + timeout
        if acked >= sent and sent < size:
            nxt = min(sent + CHUNK, size)
            dev.write(data[sent:nxt])
            sent = nxt

    crc = zlib.crc32(data) & 0xFFFFFFFF
    dev.write(struct.pack("<I", crc))
    # The CRC/rename/footer is the part that used to be given only 15 s. A
    # large book can spend longer in that tail than the data ACKs did, so this
    # window is the transfer timeout (90 s by default) and DONE *or* the
    # device's own "receive: wrote <n> bytes" line proves the file landed.
    deadline = time.time() + timeout
    while True:
        reply = _read_reply_line(dev, max(1.0, deadline - time.time()))
        if reply is None:
            raise P4Error("receive %s: no DONE after CRC (%d bytes sent)"
                          % (remote, size))
        if DONE in reply or _wrote_size(reply) == size:
            return
        error = _reply_error(reply)
        if error is not None:
            raise P4Error("receive %s: %s" % (remote, error))
        if time.time() > deadline:
            raise P4Error("receive %s: no DONE (last reply %r)"
                          % (remote, reply[-80:]))


def push_local(dev: DeviceSession, local: str, remote: Optional[str] = None,
               retries: int = 4) -> None:
    remote = remote or os.path.basename(local)
    with open(local, "rb") as fh:
        data = fh.read()
    push_file(dev, remote, data, retries=retries)


def pull_file(dev: DeviceSession, remote: str, timeout: float = 120.0) -> bytes:
    """Download ``remote`` via the ``send`` (SDFX) framing."""
    dev.reset_input()
    dev.write(("send %s\r\n" % remote).encode())
    prelude = dev.read_until(b"SDFX", timeout)
    if b"SDFX" not in prelude:
        raise P4Error("send %s: no SDFX frame (%r)" % (remote, prelude[-160:]))
    size = struct.unpack("<I", dev.read_exact(4, 15.0))[0]
    return dev.read_exact(size, timeout)
