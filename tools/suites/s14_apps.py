# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Reference-app suite: deploy and drive every app in ``apps/push_apps.py``.

The entry ``.BAT`` files go to the SD root (on PATH), their ``.APPINFO``
metadata to ``APPS/`` (the ``launch`` convention), the Companion files the same
way as ``apps/companion/push_sd.py``, and the sample sprites (SHIP / BALL /
PHOTO) are generated with the ``apps/push_assets.py`` PIL logic and pushed too.

Each app is launched and its documented marker asserted, then exited. The
menu-driven apps are driven through their serial list selection (1-based entry
number; the firmware returns the 0-based index as ERRORLEVEL). Screenshots are
taken while a modal is open for a handful of apps.

Deployed payloads are intentionally left installed (that matches the normal
push tools, and later suites may rely on them); only per-run artifacts such as
saved BMPs/TXT are cleaned up.
"""
from __future__ import annotations

import os
import shutil
import tempfile

from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "apps"
TAGS = ["apps", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")

# Mirrors apps/push_apps.py FILES: (subdir, filename, goes-to-APPS).
PUSH_APPS = [
    ("adventure", "ADVENT.BAT", False),
    ("adventure", "ADVENT.APPINFO", True),
    ("adventure", "ALIASES.BAT", False),
    ("notes", "NOTES.BAT", False),
    ("notes", "NOTES.APPINFO", True),
    ("notes", "ALIASES.BAT", False),
    ("mood", "MOOD.BAT", False),
    ("mood", "MOOD.APPINFO", True),
    ("mood", "ALIASES.BAT", False),
    ("gfxdemo", "BOUNCE.BAT", False),
    ("gfxdemo", "BOUNCE.APPINFO", True),
    ("gfxdemo", "GFXTOOL.BAT", False),
    ("gfxdemo", "GFXTOOL.APPINFO", True),
    ("gfxdemo", "PLOT.BAT", False),
    ("gfxdemo", "PLOT.APPINFO", True),
    ("snake", "SNAKE.BAT", False),
    ("snake", "SNAKE.APPINFO", True),
    ("tcmd", "TCMD.BAT", False),
    ("tcmd", "TCMD.APPINFO", True),
    ("elite", "ELITE.BAT", False),
    ("elite", "ELITE.APPINFO", True),
    ("pics", "PICS.BAT", False),
    ("pics", "PICS.APPINFO", True),
    ("uitest", "UITEST.BAT", False),
    ("uitest", "UITEST.APPINFO", True),
    ("palmtop", "PALMTOP.BAT", False),
    ("palmtop", "PALMTOP.APPINFO", True),
    ("diag", "DIAG.BAT", False),
    ("diag", "DIAG.APPINFO", True),
    ("controlflow", "CONTROL.BAT", False),
    ("controlflow", "CONTROL.APPINFO", True),
    ("writer", "WRITER.BAT", False),
    ("writer", "WRITER.APPINFO", True),
]

# Mirrors apps/companion/push_sd.py FILES.
COMPANION_FILES = [
    "LIB.BAT", "COMPANION.BAT", "SYS.BAT", "FILES.BAT", "NET.BAT", "FUN.BAT",
    "SET.BAT", "ALIASES.BAT", "SVC.BAT", "AGENDA.BAT", "README.TXT",
    "COMPANION.APPINFO",
]

# Apps that run to completion on their own: (name, start, done, timeout).
NONSTOP = [
    ("BOUNCE", "[M-BOUNCE]", "[M-BOUNCE-DONE]", 200.0),
    ("GFXTOOL", "[M-GFXTOOL]", "[M-GFXTOOL-DONE]", 120.0),
    ("PLOT", "[M-PLOT]", "[M-PLOT-DONE]", 60.0),
    ("UITEST", "[M-UITEST]", "[M-UITEST-DONE]", 60.0),
    ("CONTROL", "[M-CONTROL]", "[M-CONTROL-DONE]", 60.0),
    ("WRITER", "[M-WRITER]", "[M-WRITER-DONE]", 60.0),
]

# Interactive apps: launch, let the surface open, send an exit, then either
# wait for a done marker or prove the shell answered again.
DRIVE = [
    ("ADVENT", "[M-ADV]", "quit", "[M-ADV-QUIT]", 2.5, True),
    ("MOOD", "[M-MOOD]", "5", None, 5.0, False),
    ("TCMD", "[M-TCMD]", "q", "[M-TCMD-DONE]", 3.0, False),
    ("ELITE", "[M-ELITE-DOCKED]", "5", "[M-ELITE-DONE]", 3.0, False),
    ("PICS", "[M-PICS]", "5", None, 4.0, True),
    ("PALMTOP", "[M-PALM]", "7", None, 4.0, True),
    ("DIAG", "[M-DIAG]", "9", "[M-DIAG-DONE]", 4.0, False),
    ("COMPANION", "[C-MENU]", "6", "[C-EXIT]", 2.0, True),
]


def _run(dev, c, label, cmd, timeout=30.0, settle=0.5):
    try:
        return dev.run(cmd, timeout=timeout, settle=settle)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check(label, False, "%r did not return: %s" % (cmd, exc))
        return ""


def _shell_echo(dev, tag="P4APPOK"):
    """True only when the SHELL actually answered.

    `dev.run()` strips the submitted command's own echo line and marker-syncs
    the worker, so an app that still owns the input (TCMD's `choice`, an open
    editor) cannot make this pass by merely echoing the typed line (H1).
    """
    try:
        out = dev.run("echo " + tag, timeout=20)
        return tag in out
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        return False


def _push(dev, c, local, remote):
    try:
        dev.push_local(local, remote)
        c.check("push %s" % remote, True)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("push %s" % remote, False, str(exc))


def _deploy(dev, c):
    _run(dev, c, "mkdir APPS", "md APPS", timeout=15)
    for subdir, name, to_apps in PUSH_APPS:
        local = os.path.join(APPS, subdir, name)
        remote = ("APPS/" + name) if to_apps else name
        _push(dev, c, local, remote)
    for name in COMPANION_FILES:
        local = os.path.join(APPS, "companion", name)
        remote = ("APPS/" + name) if name.endswith(".APPINFO") else name
        _push(dev, c, local, remote)


def _make_ship(path):
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (48, 48), (0, 0, 170))
    d = ImageDraw.Draw(im)
    d.ellipse([6, 20, 42, 34], fill=(170, 170, 170))
    d.ellipse([18, 12, 30, 24], fill=(85, 255, 255))
    d.point([(10, 27), (24, 27), (38, 27)], fill=(255, 85, 85))
    d.rectangle([22, 34, 26, 38], fill=(85, 85, 85))
    im.save(path)


def _make_ball(path):
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (16, 16), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([1, 1, 14, 14], fill=(255, 255, 255))
    d.ellipse([5, 5, 10, 10], fill=(255, 0, 0))
    im.save(path)


def _make_photo(path):
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (96, 64))
    px = im.load()
    for y in range(64):
        for x in range(96):
            r = int(20 + (y / 63.0) * 200)
            g = int(60 + (y / 63.0) * 120)
            b = int(160 - (y / 63.0) * 120)
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(im)
    d.ellipse([64, 8, 84, 28], fill=(255, 230, 120))
    d.polygon([(0, 64), (30, 30), (60, 64)], fill=(30, 110, 45))
    d.polygon([(40, 64), (72, 36), (96, 64)], fill=(20, 80, 35))
    im.save(path)


def _push_sprites(dev, c):
    """Generate the push_assets.py sample art with PIL and push it."""
    try:
        from PIL import Image  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        c.note("sprite generation skipped - PIL unavailable: %s" % exc)
        return
    tmp = tempfile.mkdtemp(prefix="p4test_sprites_")
    try:
        makers = [("SHIP.BMP", _make_ship), ("BALL.BMP", _make_ball),
                  ("PHOTO.BMP", _make_photo)]
        for name, maker in makers:
            path = os.path.join(tmp, name)
            maker(path)
            _push(dev, c, path, name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _oneshot(dev, c, name, start, done, timeout):
    out = _run(dev, c, "launch %s" % name, "launch %s" % name, timeout=timeout)
    c.expect("%s start marker" % name, start, out)
    c.expect("%s done marker" % name, done, out)


def _drive(dev, c, name, start, exit_line, exit_marker, open_wait, shot):
    dev.session.reset_input()
    dev.session.write_line("launch %s" % name)
    head = dev.session.read_until(start.encode(), 60)
    c.check("%s start marker %s" % (name, start), start in head.decode("utf-8", "replace"),
            head[-300:].decode("utf-8", "replace"))
    dev.session.read_for(open_wait)
    if shot:
        try:
            dev.screenshot(out_dir=OUT_DIR, name="apps_%s" % name.lower())
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.note("%s screenshot failed: %s" % (name, exc))
    if exit_line is not None:
        dev.session.write_line(exit_line)
    if exit_marker is not None:
        tail = dev.session.read_until(exit_marker.encode(), 40)
        c.check("%s exit marker %s" % (name, exit_marker),
                exit_marker in tail.decode("utf-8", "replace"),
                tail[-300:].decode("utf-8", "replace"))
    else:
        c.check("%s returned to the shell" % name, _shell_echo(dev), "")


def _tcmd(dev, c):
    """Launch TCMD and guarantee it fully exits before the next app.

    TCMD's documented quit key is ``q`` (then Enter), but it sits in a
    ``choice`` key loop that is re-armed after every refresh. A key sent
    while the refresh is in flight is dispatched as a shell command and the
    loop then times out with the app still open, which used to eat every later
    ``launch`` (H7). Sync on the ``key?`` prompt before each quit key, retry,
    and force the quit in a ``finally`` if it is somehow left open.
    """
    _, start, exit_line, exit_marker, open_wait, _ = DRIVE[2]
    session = dev.session
    session.reset_input()
    session.write_line("launch TCMD")
    head = session.read_until(start.encode(), 60)
    c.check("TCMD start marker %s" % start,
            start in head.decode("utf-8", "replace"),
            head[-300:].decode("utf-8", "replace"))
    session.read_for(open_wait)

    key = (exit_line or "q")
    exited = False
    try:
        session.read_until(b"key?", 40)
        for _attempt in range(4):
            session.write_line(key)
            tail = session.read_until(exit_marker.encode(), 10)
            if exit_marker in tail.decode("utf-8", "replace"):
                exited = True
                break
            session.read_until(b"key?", 30)
        c.check("TCMD quit marker %s" % exit_marker, exited,
                "TCMD did not exit after %r" % key)
        if exited:
            c.check("TCMD returned to the shell", _shell_echo(dev), "")
    finally:
        if not exited:
            # Best effort: keep offering the quit key until the shell answers,
            # so the next app's launch is not consumed by TCMD's key wait.
            for _ in range(4):
                session.write_line(key)
                if _shell_echo(dev):
                    break
                session.read_for(0.5)


def _snake(dev, c):
    dev.session.reset_input()
    dev.session.write_line("launch SNAKE")
    head = dev.session.read_until(b"[M-SNAKE]", 60)
    c.check("SNAKE start marker", b"[M-SNAKE]" in head,
            head[-300:].decode("utf-8", "replace"))
    dev.session.read_for(3.0)
    dev.session.write_line("q")
    tail = dev.session.read_until(b"[M-SNAKE-DONE]", 40)
    c.check("SNAKE done marker", b"[M-SNAKE-DONE]" in tail,
            tail[-300:].decode("utf-8", "replace"))


def _notes(dev, c):
    dev.session.reset_input()
    dev.session.write_line("launch NOTES")
    head = dev.session.read_until(b"[M-NOTE]", 60)
    c.check("NOTES start marker", b"[M-NOTE]" in head,
            head[-300:].decode("utf-8", "replace"))
    # First run creates the store and pops a dialog; dismiss it, then Back.
    dev.session.read_for(12.0)
    dev.session.write_line("ok")
    dev.session.read_for(2.0)
    try:
        dev.screenshot(out_dir=OUT_DIR, name="apps_notes")
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("NOTES screenshot failed: %s" % exc)
    # "Back" is the 8th menu entry; send twice so a late first-run dialog
    # that swallowed the first line cannot leave the menu open.
    dev.session.write_line("8")
    dev.session.read_for(2.0)
    dev.session.write_line("8")
    c.check("NOTES returned to the shell", _shell_echo(dev), "")


def _cleanup(dev, c):
    artifacts = [
        "BOUNCE.BMP", "SNAKE.BMP", "GFXTOOL.BMP", "PLOT.BMP", "TCMD.BMP",
        "DIAG_BENCH.BMP", "ELITE.BMP", "DIAG.TXT", "ADVENT.SAV", "ELITE.SAV",
        "_z.txt", "_zinit.txt", "_m.txt", "_s.txt", "_tz.txt", "_d.txt",
        "_mood.txt", "_LT.txt", "_RT.txt", "_palm.csv",
    ]
    for name in artifacts:
        try:
            dev.run("del %s" % name, timeout=15)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    for cmd in ("draw close", "taskkill bg0", "gfx close"):
        try:
            dev.run(cmd, timeout=15)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    c.note("cleanup complete; deployed app payloads left installed")


def run(dev, ctx):
    c = Checklist(NAME)
    quick = bool(ctx.get("quick"))

    # Defensive: TUI mode (owned by `draw`) can still be active from a previous
    # app/run, and then BOUNCE/GFXTOOL refuse with "gfx: TUI mode is active".
    # Close it before touching the display.
    try:
        dev.run("draw close", timeout=15)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass

    _deploy(dev, c)
    _push_sprites(dev, c)

    listed = _run(dev, c, "launch /list", "launch /list", timeout=25)
    c.expect("launch lists deployed apps", "BOUNCE", listed)
    c.expect("launch lists the companion", "COMPANION", listed)

    for name, start, done, timeout in NONSTOP:
        _oneshot(dev, c, name, start, done, timeout)

    _drive(dev, c, *DRIVE[0])
    _snake(dev, c)
    _drive(dev, c, *DRIVE[1])
    _tcmd(dev, c)
    _notes(dev, c)
    for entry in DRIVE[3:]:
        _drive(dev, c, *entry)

    if quick:
        c.note("quick mode: no extra long-app interaction")

    _cleanup(dev, c)
    return c
