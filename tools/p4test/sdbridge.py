# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Byte-exact SD transfers over the serial link.

``receive <path> <size> /crc`` and ``send <path>`` are the firmware's own
binary channels. Both are wrapped here once; the deployment tools and every
suite call these instead of re-implementing ACK pacing or SDFX framing.
"""
from __future__ import annotations

import os
import struct
import time
import zlib
from typing import Optional

from .session import DeviceSession, P4Error

CHUNK = 4096
READY = b"=== RX READY ==="
DONE = b"=== RX DONE ==="


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

    sent = 0
    dev.write(data[:CHUNK])
    sent = min(CHUNK, size)
    while True:
        ack = dev.read_until(b"\n", timeout)
        if not ack:
            raise P4Error("receive %s: no ACK" % remote)
        ack = ack.replace(b"\r", b"")
        if DONE in ack:
            return
        lines = [ln for ln in ack.split(b"\n") if ln.strip().startswith(b"RX ")]
        if not lines:
            raise P4Error("receive %s: bad ACK %r" % (remote, ack[-80:]))
        cum = int(lines[-1].strip()[3:])
        if cum >= size:
            break
        if sent < size:
            nxt = min(sent + CHUNK, size)
            dev.write(data[sent:nxt])
            sent = nxt

    crc = zlib.crc32(data) & 0xFFFFFFFF
    dev.write(struct.pack("<I", crc))
    done = dev.read_until(DONE, 15.0)
    if DONE not in done:
        raise P4Error("receive %s: no DONE (%r)" % (remote, done[-120:]))


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
