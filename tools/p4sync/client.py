# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync client: thin operations over :class:`p4test.device.Device`.

Every byte on the wire goes through the shared `p4test` session driver
(DTR-safe open, marker sync, panic detection, SDFX/BMPX framing). This
module adds no serial logic of its own.
"""
from __future__ import annotations

import os
import sys
from typing import Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from p4test.device import Device  # noqa: E402

PROTO_TAG = "p4sync-1"


class P4SyncClient:
    """One-device sync session. Use as a context manager."""

    def __init__(self, port: Optional[str] = None, quick: bool = False):
        self.dev = Device(port, boot=True, settle_ms=4000 if quick else 8000)

    def close(self) -> None:
        self.dev.close()

    def __enter__(self) -> "P4SyncClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- handshake ------------------------------------------------------
    def status(self) -> str:
        """The raw `sync status` text (ANSI-stripped by the session)."""
        return self.dev.run("sync status", timeout=25)

    def check_status(self) -> Tuple[bool, str]:
        """True when the handshake carries the expected proto tag."""
        out = self.status()
        return (PROTO_TAG in out, out)

    def board_id(self) -> str:
        return self.dev.board_id()

    def display_size(self) -> Tuple[int, int]:
        return self.dev.display_size()

    # -- files (receive/send framing inside sdbridge) --------------------
    def push_file(self, remote: str, data: bytes) -> None:
        self.dev.push_file(remote, data)

    def push_local(self, local: str, remote: Optional[str] = None) -> None:
        self.dev.push_local(local, remote)

    def pull_file(self, remote: str) -> bytes:
        return self.dev.pull_file(remote)

    # -- screen (BMPX framing inside screenshot) --------------------------
    def screenshot(self, out_path: Optional[str] = None):
        """Capture the live screen; optionally save a PNG proof."""
        shot = self.dev.screenshot()
        if out_path:
            shot.save_png(out_path)
        return shot
