# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""p4test - shared host-side test framework for P4MiniShell.

One package replaces the copy-pasted serial loops, BMPX/SDFX readers, and
ad-hoc PASS/FAIL bookkeeping that grew across ``tools/``. Every suite and
driver imports from here:

* :class:`p4test.session.DeviceSession` - DTR-safe serial session with
  marker-synchronised command execution, panic detection, and raw/binary I/O.
* :class:`p4test.asserts.Checklist` - named PASS/FAIL assertions and tables.
* :func:`p4test.screenshot.capture` - streaming BMP screenshot + pixel checks.
* :func:`p4test.sdbridge.push_file` / ``pull_file`` - ACK-paced SD transfer.
* :mod:`p4test.perf` - frame-timing / smoothness measurement.
* :class:`p4test.device.Device` - high-level facade (boot, deploy, run, shoot).
* :mod:`p4test.runner` - suite discovery, orchestration, and reporting.
"""

from .asserts import Checklist, SuiteResult  # noqa: F401
from .session import DeviceSession, P4Error, PanicError  # noqa: F401

__all__ = [
    "Checklist",
    "SuiteResult",
    "DeviceSession",
    "P4Error",
    "PanicError",
]
