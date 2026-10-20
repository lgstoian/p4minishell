# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Local, git-ignored bench configuration (never committed).

The only file this reads is ``tools/wifi_local.json`` next to the tools dir::

    { "ssid": "camera", "password": "..." }

It is listed in ``.gitignore`` so a bench credential never lands in git.
Suites and drivers that need a live AP call :func:`wifi_credentials`; when the
file is absent they get ``(None, None)`` and skip the live leg cleanly.
"""
from __future__ import annotations

import json
import os
from typing import Optional, Tuple


def _tools_dir() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def wifi_local_path() -> str:
    return os.path.join(_tools_dir(), "wifi_local.json")


def wifi_credentials() -> Tuple[Optional[str], Optional[str]]:
    """Return ``(ssid, password)`` from ``tools/wifi_local.json`` or (None, None)."""
    try:
        with open(wifi_local_path(), "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return (None, None)
    ssid = data.get("ssid") or None
    password = data.get("password") or None
    return (ssid, password)
