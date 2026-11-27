# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync host prototype: USB desktop-sync client over the shared transports.

Wraps :class:`p4test.device.Device` (DTR-safe open, marker-synced runs,
SDFX push/pull, BMPX screenshots) with the P4Sync operation set: handshake,
file push/pull, and screenshot capture. PIM merge and package install stay
with their owners (`tools/pim_sync.py`, `pkg`/`asset` verbs); this client
only asserts the `sync status` handshake advertises the magics and limits
those paths depend on, so no second framing reader or merge lives here.
"""
__all__ = ["P4SyncClient"]


def __getattr__(name):
    """Load the serial-backed client only when it is actually requested.

    ``bridge.py --mock`` must import the pure protocol layer on a machine with
    no pyserial; eager package import used to pull in the whole harness.
    """
    if name == "P4SyncClient":
        from .client import P4SyncClient
        return P4SyncClient
    raise AttributeError("module %r has no attribute %r" % (__name__, name))
