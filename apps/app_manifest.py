#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""app_manifest.py - single source of truth for the reference-app payloads.

The batch apps used to be listed separately in ``apps/push_apps.py``,
``apps/companion/push_sd.py`` and ``tools/suites/s14_apps.py``. Keeping three
copies in sync was a recurring source of "the app is there but the test never
deployed the new file" failures. This module is the one list; the tools import
it (``sys.path`` already includes ``apps/`` for the push scripts, and the suite
adds it explicitly).

A payload is ``(subdir, filename, target)``:

* ``subdir`` is relative to ``apps/`` (``""`` for the shared root payloads).
* ``target`` is ``"root"`` (SD root, on PATH so ``launch``/``gosub`` find it)
  or ``"apps"`` (``sd:/APPS``, the ``launch`` metadata directory).
"""

import os

APPS_DIR = os.path.dirname(os.path.abspath(__file__))

# (subdir, filename, target) - reference apps + shared payloads.
APP_PAYLOADS = [
    # Shared routine library every entry app gosubs into.
    ("", "APPLIB.BAT", "root"),

    ("adventure", "ADVENT.BAT", "root"),
    ("adventure", "ADVENT.APPINFO", "apps"),
    ("adventure", "ALIASES.BAT", "root"),

    ("chat", "CHAT.BAT", "root"),
    ("chat", "CHAT.APPINFO", "apps"),

    # Browser showcase: the entry shim + the static site it serves.
    ("website", "WEB.BAT", "root"),
    ("website", "WEB.APPINFO", "apps"),
    ("website", "INDEX.HTML", "web"),
    ("website", "PAGE2.HTML", "web"),

    ("notes", "NOTES.BAT", "root"),
    ("notes", "NOTES.APPINFO", "apps"),
    ("notes", "ALIASES.BAT", "root"),

    ("mood", "MOOD.BAT", "root"),
    ("mood", "MOOD.APPINFO", "apps"),
    ("mood", "MOODSVC.BAT", "root"),
    ("mood", "MOODSVC.APPINFO", "apps"),
    ("mood", "ALIASES.BAT", "root"),

    ("gfxdemo", "BOUNCE.BAT", "root"),
    ("gfxdemo", "BOUNCE.APPINFO", "apps"),
    ("gfxdemo", "GFXTOOL.BAT", "root"),
    ("gfxdemo", "GFXTOOL.APPINFO", "apps"),
    ("gfxdemo", "PLOT.BAT", "root"),
    ("gfxdemo", "PLOT.APPINFO", "apps"),

    ("snake", "SNAKE.BAT", "root"),
    ("snake", "SNAKE.APPINFO", "apps"),
    ("tcmd", "TCMD.BAT", "root"),
    ("tcmd", "TCMD.APPINFO", "apps"),
    ("elite", "ELITE.BAT", "root"),
    ("elite", "ELITE.APPINFO", "apps"),
    ("pics", "PICS.BAT", "root"),
    ("pics", "PICS.APPINFO", "apps"),
    ("uitest", "UITEST.BAT", "root"),
    ("uitest", "UITEST.APPINFO", "apps"),
    ("palmtop", "PALMTOP.BAT", "root"),
    ("palmtop", "PALMTOP.APPINFO", "apps"),
    ("diag", "DIAG.BAT", "root"),
    ("diag", "DIAG.APPINFO", "apps"),
    ("controlflow", "CONTROL.BAT", "root"),
    ("controlflow", "CONTROL.APPINFO", "apps"),
    ("writer", "WRITER.BAT", "root"),
    ("writer", "WRITER.APPINFO", "apps"),
    ("hybrid", "HYBRID.BAT", "root"),
    ("hybrid", "HYBRID.APPINFO", "apps"),
    ("taskmgr", "TASKMGR.BAT", "root"),
    ("taskmgr", "TASKMGR.APPINFO", "apps"),
    ("pkgtest", "PKGTEST.BAT", "root"),
    ("pkgtest", "PKGTEST.APPINFO", "apps"),
]

# Companion menu + sub-apps; ``push_sd.py`` deploys only this group.
COMPANION_PAYLOADS = [
    ("companion", name, "apps" if name.endswith(".APPINFO") else "root")
    for name in (
        "LIB.BAT", "COMPANION.BAT", "SYS.BAT", "FILES.BAT", "NET.BAT",
        "FUN.BAT", "SET.BAT", "ALIASES.BAT", "SVC.BAT", "MEMLOG.BAT",
        "AGENDA.BAT", "README.TXT", "COMPANION.APPINFO",
    )
]


def payload_path(subdir, filename):
    """Absolute local path for one payload."""
    return os.path.join(APPS_DIR, subdir, filename) if subdir else os.path.join(APPS_DIR, filename)


def remote_target(filename, target):
    """SD path for one payload.

    ``apps`` -> ``APPS/<name>`` (launcher metadata), ``web`` -> ``WEB/<name>``
    (the browser showcase site), ``root`` -> the filename at the SD root.
    """
    if target == "apps":
        return "APPS/" + filename
    if target == "web":
        return "WEB/" + filename
    return filename


def deploy_list(groups=("apps", "companion")):
    """Yield ``(local_path, remote_path)`` for the requested groups."""
    selected = []
    if "apps" in groups:
        selected += APP_PAYLOADS
    if "companion" in groups:
        selected += COMPANION_PAYLOADS
    for subdir, filename, target in selected:
        yield payload_path(subdir, filename), remote_target(filename, target)
