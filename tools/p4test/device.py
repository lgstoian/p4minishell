# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Device - a high-level facade over :class:`DeviceSession`.

Bundles the operations every suite and deployment tool needs: boot-and-quiesce,
run a command, capture the screen, push/pull SD files, and drive the EXISTING
destructive commands (``config factory`` / ``format``) that the firmware
already provides - no duplicated reset logic lives here.
"""
from __future__ import annotations

import os
import re
import time
from typing import Dict, Iterable, Optional, Sequence, Tuple

from . import screenshot as shot
from . import sdbridge
from .session import DeviceSession, P4Error, strip_ansi


class Device:
    def __init__(self, port: Optional[str] = None, baud: int = 115200,
                 boot: bool = True, settle_ms: int = 8000):
        self.session = DeviceSession(port, baud)
        if boot:
            self.boot(settle_ms)

    # -- lifecycle -------------------------------------------------------
    def boot(self, settle_ms: int = 8000) -> None:
        """Wait until the shell is genuinely ready, not just printing.

        Boot is not over when the banner appears: the Wi-Fi/C6 workflow runs on
        the command worker afterwards and queues a burst of lines (a saved
        network that is unreachable retries with 1/2/4/8/16 s delays for up to
        half a minute). A fixed settle let suites start while the worker was
        still chewing that burst, so a command's marker arrived tens of seconds
        late and the suite failed somewhere unrelated.

        `wifi status` reports `state: starting` while that workflow runs, so
        poll until it is no longer starting, then require a quiet console and
        prove it with the deterministic `delay` marker.
        """
        self.session.wait_prompt(timeout=45.0)
        deadline = time.time() + 300.0
        quiet_state = ("starting", "stopping", "unknown")
        while time.time() < deadline:
            try:
                status = self.session.run("wifi status", timeout=30)
            except P4Error:
                time.sleep(2.0)
                continue
            match = re.search(r"state:\s*(\w+)", status)
            state = match.group(1) if match else "unknown"
            if state not in quiet_state:
                break
            time.sleep(2.0)

        # Let the trailing lines of the workflow drain, then prove the worker
        # is free: a `delay` marker only prints when the command actually ran.
        last = time.time()
        while time.time() < deadline and time.time() - last < 3.0:
            if self.session.read_for(0.3):
                last = time.time()
        if settle_ms > 0:
            for attempt in range(4):
                try:
                    # Generous: the marker only prints once the worker is free,
                    # and a slow C6/Wi-Fi bring-up can hold it for a while.
                    self.session.run("delay %d" % settle_ms,
                                     timeout=settle_ms / 1000.0 + 60)
                    break
                except P4Error:
                    if attempt == 4:
                        raise
                    time.sleep(2.0)

    def reboot(self, settle_ms: int = 8000) -> None:
        self.session.hard_reset()
        self.boot(settle_ms)

    def close(self) -> None:
        self.session.close()

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- commands / screen ----------------------------------------------
    def run(self, cmd: str, timeout: float = 30.0, settle: float = 0.5) -> str:
        return self.session.run(cmd, timeout=timeout, settle=settle)

    def send(self, text: str, settle: float = 0.4) -> str:
        return self.session.send(text, settle)

    def read_for(self, seconds: float) -> str:
        return self.session.read_for(seconds)

    def screenshot(self, out_dir: Optional[str] = None,
                   name: Optional[str] = None,
                   unwind: bool = True) -> shot.Bmp:
        return shot.capture(self.session, out_dir, name, unwind=unwind)

    # -- cursor simulation + links (serial-driven, same indev as HID) ----
    def cursor_move(self, x: int, y: int, timeout: float = 20.0) -> str:
        """Place the shared mouse cursor at native-panel (x, y)."""
        return self.run("usb mouse move %d %d" % (x, y), timeout=timeout)

    def cursor_click(self, hold_ms: int = 80, timeout: float = 20.0) -> str:
        """Synthesize one click at the tracked cursor position."""
        return self.run("usb mouse click %d" % hold_ms, timeout=timeout)

    def ui_links(self, timeout: float = 20.0) -> str:
        """List transcript link/anchor regions (dense indices)."""
        return self.run("ui links", timeout=timeout)

    def ui_link(self, index: int, timeout: float = 20.0) -> str:
        """Semantically activate link <index> (confirm-gated like a tap)."""
        return self.run("ui link %d" % index, timeout=timeout)

    # -- board capabilities ---------------------------------------------
    def sysinfo(self) -> str:
        """`sysinfo` text (ANSI-stripped), cached for the session."""
        cached = getattr(self, "_sysinfo_cache", None)
        if cached is None:
            cached = strip_ansi(self.run("sysinfo", timeout=25))
            self._sysinfo_cache = cached
        return cached

    def board_id(self) -> str:
        """The active board profile slug (e.g. ``jc1060p470c``)."""
        m = re.search(r"board\.id:\s*([A-Za-z0-9_\-]+)", self.sysinfo())
        return m.group(1) if m else ""

    def display_size(self) -> Tuple[int, int]:
        """The live landscape display size as ``(width, height)``.

        Declared per board: 1024x600 on the reference board, 1280x720 on the
        Tab5. Suites must assert against this rather than a hardcoded panel.
        """
        m = re.search(r"display:\s*(\d+)\s*x\s*(\d+)", self.sysinfo())
        if not m:
            return (1024, 600)
        return (int(m.group(1)), int(m.group(2)))

    def rgb_available(self) -> bool:
        """True when the board exposes a controllable WS2812 status LED.

        The Tab5 has no on-board LED; its keyboard adds one when attached, so
        the firmware reports ``hardware.rgb: gpio=-1`` without it.
        """
        m = re.search(r"hardware\.rgb:\s*gpio=(-?\d+)", self.sysinfo())
        return bool(m and int(m.group(1)) >= 0)

    # -- SD transfer -----------------------------------------------------
    def push_file(self, remote: str, data: bytes) -> None:
        sdbridge.push_file(self.session, remote, data)

    def push_local(self, local: str, remote: Optional[str] = None) -> None:
        sdbridge.push_local(self.session, local, remote)

    def pull_file(self, remote: str) -> bytes:
        return sdbridge.pull_file(self.session, remote)

    def deploy(self, files: Iterable[Tuple[str, str]],
               appinfo_dir: str = "APPS") -> None:
        """Push ``(local_path, remote_path)`` pairs, making parent dirs.

        ``.APPINFO``/``.ASSETS`` targets default under ``appinfo_dir`` when the
        caller passes a bare name (matching the ``launch``/``pkg`` convention).
        """
        made = set()
        for local, remote in files:
            parent = os.path.dirname(remote)
            if parent and parent not in made:
                self.run("md %s" % parent.replace("/", "\\"), timeout=10)
                made.add(parent)
            self.push_local(local, remote)

    # -- destructive commands (existing firmware, driven verbatim) -------
    def factory_reset(self, timeout: float = 30.0) -> str:
        self.session.reset_input()
        self.session.write_line("config factory")
        if b"Type YES" not in self.session.read_until(b"Type YES to continue:", 12):
            raise P4Error("config factory: no confirmation prompt")
        self.session.write_line("YES")
        out = self.session.read_until(b"factory reset complete", timeout)
        if b"factory reset complete" not in out:
            raise P4Error("config factory did not report completion")
        return out.decode("utf-8", "replace")

    def format_sd(self, label: str = "P4MINISHELL", timeout: float = 600.0) -> str:
        # A full FAT32 layout of a large card is synchronous and slow (tens of
        # seconds up to a few minutes); the old 180 s wait produced false
        # "format did not report completion" failures.
        self.session.reset_input()
        self.session.write_line(("format /V:%s" % label) if label else "format")
        if b"Type YES" not in self.session.read_until(b"Type YES to continue:", 12):
            raise P4Error("format: no confirmation prompt")
        self.session.write_line("YES")
        out = self.session.read_until(b"format: complete", timeout)
        if b"format: complete" not in out:
            raise P4Error("format did not report completion after %.0fs" % timeout)
        return out.decode("utf-8", "replace")
