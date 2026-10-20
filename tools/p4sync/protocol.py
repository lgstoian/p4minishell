# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync wire contract (single source of truth).

Both the CLI (`tools/p4sync`) and the future desktop app import these
constants. Nothing here talks to hardware - it only names the contract the
firmware already exposes through `sync status` and the SDFX/PIMX/BMPX frames.

Protocol shape:
    host -> `sync status`
    device -> key=value capability lines (see CAP_KEYS)
    host -> `receive <path>` / `send <path>` per file (SDFX framing)
    host -> `pim get|put ...` (PIMX framing)
    host -> `screenshot` (BMPX framing)

Refuse to operate when the advertised proto tag does not match: never guess a
framing or a capability from a different firmware revision.
"""
from __future__ import annotations

from typing import Dict

PROTO_TAG = "p4sync-1"

# `sync status` capability keys the client relies on. Keep this list in lockstep
# with components/command/sync_commands.c; an absent key is a hard error, not a
# default.
CAP_KEYS = (
    "proto",      # p4sync-N
    "board",      # board profile slug
    "sd",         # ready / absent
    "lock",       # locked / unlocked
    "magic.sdfx", # transfer framing magic
    "magic.pimx",
    "magic.bmpx",
    "limit.transfer",  # max bytes per receive/send
    "limit.screenshot",
)

MANIFEST_VERSION = 1


def parse_sync_status(text: str) -> Dict[str, str]:
    """Parse `sync status` into a key->value dict (ANSI already stripped).

    Lines are `key: value` / `key=value`; unknown keys are kept verbatim so the
    caller can report drift without a second parser.
    """
    caps: Dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("P4Sync", "Sync ")):
            continue
        for sep in (":", "="):
            if sep in line:
                key, _, value = line.partition(sep)
                key = key.strip()
                if key:
                    caps[key] = value.strip()
                break
    return caps


def require_capabilities(caps: Dict[str, str], tag: str = PROTO_TAG) -> str:
    """Return '' when the handshake is compatible, else a human error string."""
    if caps.get("proto") != tag:
        return "proto mismatch: device=%r host=%r" % (caps.get("proto"), tag)
    missing = [k for k in CAP_KEYS if k not in caps]
    if missing:
        return "missing capabilities: %s" % ", ".join(missing)
    return ""
