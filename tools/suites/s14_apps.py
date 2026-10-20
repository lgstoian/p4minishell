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
import sys
import tempfile
import time

from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "apps"
TAGS = ["apps", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
sys.path.insert(0, APPS)

# The deploy list is the single source in apps/app_manifest.py (shared with
# push_apps.py and companion/push_sd.py). `APP_PAYLOADS` includes the shared
# APPLIB.BAT and the CHAT app; `COMPANION_PAYLOADS` covers the menu tree.
from app_manifest import (  # noqa: E402
    APP_PAYLOADS,
    COMPANION_PAYLOADS,
    payload_path,
    remote_target,
)



# Apps that run to completion on their own: (name, start, done, timeout,
# extra-must-see). The `extra` list pins the post-authoring feature lines
# (and markers) that the app must have emitted on its way to `done`.
NONSTOP = [
    ("HYBRID", "[M-HYBRID]", "[M-HYBRID-DONE]", 60.0, []),
    ("BOUNCE", "[M-BOUNCE]", "[M-BOUNCE-DONE]", 200.0, ["[M-BOUNCE-FRAME]"]),
    ("GFXTOOL", "[M-GFXTOOL]", "[M-GFXTOOL-DONE]", 120.0, ["[M-GFXTOOL-BLIT]"]),
    ("PLOT", "[M-PLOT]", "[M-PLOT-DONE]", 60.0, []),
    ("UITEST", "[M-UITEST]", "[M-UITEST-DONE]", 60.0, []),
    ("CONTROL", "[M-CONTROL]", "[M-CONTROL-DONE]", 60.0,
     ["CONTROL: switch diag", "CONTROL: while 3", "CONTROL: if-group taken",
      "CONTROL: fora 0=alpha", "CONTROL: fora 1=beta",
      "CONTROL: substr cde replace Abcdefgh", "CONTROL: delayed now",
      "Parity: dot ok", "Parity: slash ok", "Parity: paren ok",
      "Parity: colon ok", "CONTROL: nul-ok", "CONTROL: handle-merge",
      "CONTROL: stdin-type ok", "CONTROL: shift-2 a-c",
      "CONTROL: eol-keep ;kept"]),
    ("WRITER", "[M-WRITER]", "[M-WRITER-DONE]", 60.0, []),
]

# Plain drives (smoke + screenshot): (name, start, exit_line, exit_marker,
# open_wait, shot). The interaction-heavy apps each get a dedicated deep
# driver below instead.
DRIVE = [
    ("PICS", "[M-PICS]", "5", None, 4.0, True),
    ("COMPANION", "[C-MENU]", "6", "[C-EXIT]", 2.0, True),
    ("CHAT", "[M-CHAT]", "9", "[M-CHAT-DONE]", 2.0, False),
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


def _wait(session, marker, timeout=45.0):
    """Wait for one marker; the session keeps the tail for the next wait."""
    got = session.read_until(marker.encode(), timeout)
    return marker.encode() in got


def _press(session, line, marker, timeout=45.0, settle=3.0):
    """Send one serial line after the targeted modal has had time to open,
    then wait for `marker`. Retries once when the line was dropped (the send
    raced the modal still opening and was routed to the shell instead).
    Returns the captured tail (marker presence = success)."""
    session.read_for(settle)
    session.write_line(line)
    got = session.read_until(marker.encode(), timeout)
    if marker.encode() in got:
        return got
    session.read_for(2.0)
    session.write_line(line)
    got += session.read_until(marker.encode(), timeout)
    if marker.encode() in got:
        return got
    return got


def _ok(session, marker=None, settle=2.0):
    """Dismiss a dialog with `ok`; optionally wait for a marker after it."""
    session.read_for(settle)
    session.write_line("ok")
    if marker is not None:
        return _wait(session, marker)
    session.read_for(2.0)
    return True


def _screenshot(dev, c, name):
    # unwind=False: the shots fire while the app under test owns the serial
    # input, and the capture retry's foreground break (Ctrl-C) aborts a batch
    # at a line modal. That killed ADVENT's `ask` loop (and TCMD's key-loop
    # state) in every full-suite run while standalone runs passed. A missed
    # decorative shot is a note; a killed app fails the whole section.
    try:
        dev.screenshot(out_dir=OUT_DIR, name=name, unwind=False)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("%s screenshot failed: %s" % (name, exc))


def _push(dev, c, local, remote):
    try:
        dev.push_local(local, remote)
        c.check("push %s" % remote, True)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("push %s" % remote, False, str(exc))


def _deploy(dev, c):
    # Ensure sd:/APPS exists. This is idempotent setup (not a graded check) and
    # is retried: the preceding suites can leave deferred output in flight, and
    # a single marker desync on `md APPS` must not fail the suite.
    for _ in range(3):
        try:
            dev.run("md APPS", timeout=20)
            dev.run("md WEB", timeout=20)
            break
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            time.sleep(1.0)
    for subdir, name, target in APP_PAYLOADS + COMPANION_PAYLOADS:
        _push(dev, c, payload_path(subdir, name), remote_target(name, target))


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


def _oneshot(dev, c, name, start, done, timeout, extra):
    out = _run(dev, c, "launch %s" % name, "launch %s" % name, timeout=timeout)
    c.expect("%s start marker" % name, start, out)
    c.expect("%s done marker" % name, done, out)
    for pat in extra:
        c.expect("%s extra %s" % (name, pat), pat, out)


def _drive(dev, c, name, start, exit_line, exit_marker, open_wait, shot):
    dev.session.reset_input()
    dev.session.write_line("launch %s" % name)
    head = dev.session.read_until(start.encode(), 60)
    c.check("%s start marker %s" % (name, start), start in head.decode("utf-8", "replace"),
            head[-300:].decode("utf-8", "replace"))
    dev.session.read_for(open_wait)
    if shot:
        _screenshot(dev, c, "apps_%s" % name.lower())
    if exit_line is not None:
        dev.session.write_line(exit_line)
    if exit_marker is not None:
        tail = dev.session.read_until(exit_marker.encode(), 40)
        c.check("%s exit marker %s" % (name, exit_marker),
                exit_marker in tail.decode("utf-8", "replace"),
                tail[-300:].decode("utf-8", "replace"))
    else:
        c.check("%s returned to the shell" % name, _shell_echo(dev), "")


def _lib(dev, c):
    """Headless LIB.BAT selftest: the rigged app-less routines must exercise
    the substring/replace expansions and the `for /A` index/value binding."""
    out = _run(dev, c, "lib selftest", "call LIB.BAT::selftest_all", timeout=30)
    for pat in ("SELFTEST done", "SELFTEST substr=[clou]",
                "SELFTEST replace=[cloudP4]", "SELFTEST fora 0=alpha",
                "SELFTEST fora 1=beta"):
        c.expect("LIB %s" % pat, pat, out)


def _advent(dev, c):
    """A deterministic victory-free walk: switch move map, the corrected take
    guards, ITEM[] inventory and a save round-trip."""
    s = dev.session
    s.reset_input()
    s.write_line("launch ADVENT")
    head = s.read_until(b"[M-ADV]", 60)
    c.check("ADVENT start marker", b"[M-ADV]" in head,
            head[-300:].decode("utf-8", "replace"))
    _screenshot(dev, c, "apps_advent")

    # Every prompt echoes `/raw ---`; that line is the ask-ready barrier.
    got = _press(s, "n", "---")
    c.check("ADVENT move north", b"---" in got, got[-200:].decode("utf-8", "replace"))
    got = _press(s, "w", "---")
    c.check("ADVENT move west", b"---" in got, got[-200:].decode("utf-8", "replace"))
    got = _press(s, "take key", "---")
    c.check("ADVENT take key", b"You take the brass key." in got,
            got[-300:].decode("utf-8", "replace"))
    got = _press(s, "inv", "---")
    c.check("ADVENT inventory walk", b"brass key" in got,
            got[-300:].decode("utf-8", "replace"))
    got = _press(s, "score", "---")
    c.check("ADVENT score", b"Score: 10" in got, got[-200:].decode("utf-8", "replace"))
    got = _press(s, "save", "[M-ADV-SAVED]")
    c.check("ADVENT save marker", b"[M-ADV-SAVED]" in got,
            got[-300:].decode("utf-8", "replace"))
    _ok(s)
    got = _press(s, "quit", "[M-ADV-QUIT]")
    c.check("ADVENT quit marker", b"[M-ADV-QUIT]" in got,
            got[-300:].decode("utf-8", "replace"))


def _mood(dev, c):
    """Drive the 5-round while/gosub sampler to the trend screen, then back
    out. The last send is doubled: the first may have raced the list opening."""
    s = dev.session
    s.reset_input()
    s.write_line("launch MOOD")
    head = s.read_until(b"[M-MOOD]", 60)
    c.check("MOOD start marker", b"[M-MOOD]" in head,
            head[-300:].decode("utf-8", "replace"))
    tail = _press(s, "1", "[M-MOOD-TREND]", timeout=90.0, settle=4.0)
    c.check("MOOD trend marker", b"[M-MOOD-TREND]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    c.check("MOOD sampled each round", b"[M-MOOD-SAMPLE]" in tail, "")
    _ok(s)
    s.read_for(3.0)
    # Service sampler view (item 5). A sampler MAY still be publishing
    # MOODLAST.INI from an earlier run on this board, so accept either the live
    # view ([M-MOOD-SVC]) or the empty path ([M-MOODSVC-NONE]); both end in a
    # "press any key" pause that must be dismissed before the menu is usable.
    s.read_for(3.0)
    s.write_line("5")
    tail = s.read_until(b"[M-MOOD-SVC]", 20)
    if b"[M-MOOD-SVC]" not in tail and b"[M-MOODSVC-NONE]" not in tail:
        tail += s.read_until(b"[M-MOODSVC-NONE]", 20)
    c.check("MOOD service view responds (sampler or empty)",
            b"[M-MOOD-SVC]" in tail or b"[M-MOODSVC-NONE]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    s.write_line("")   # dismiss the pause
    s.read_for(2.0)
    _ok(s)
    s.read_for(3.0)
    s.write_line("6")   # Back
    s.read_for(2.0)
    s.write_line("6")   # battery: first may have missed the menu list
    c.check("MOOD returned to the shell", _shell_echo(dev), "")


def _tcmd(dev, c):
    """Launch TCMD and guarantee it fully exits before the next app.

    TCMD's documented quit key is ``q`` (then Enter), but it sits in a
    ``choice`` key loop that is re-armed after every refresh. A key sent
    while the refresh is in flight is dispatched as a shell command and the
    loop then times out with the app still open, which used to eat every later
    ``launch`` (H7). Sync on the ``key?`` prompt before each key, retry, and
    force the quit in a ``finally`` if it is somehow left open. Before the
    quit, one swap drives the on-dispatch key poll to an ``[M-TCMD-OP]``.
    """
    start = "[M-TCMD]"
    exit_marker = "[M-TCMD-DONE]"
    session = dev.session
    session.reset_input()
    session.write_line("launch TCMD")
    head = session.read_until(start.encode(), 60)
    c.check("TCMD start marker %s" % start,
            start in head.decode("utf-8", "replace"),
            head[-300:].decode("utf-8", "replace"))
    session.read_for(3.0)
    _screenshot(dev, c, "apps_tcmd")

    # Swap panes through the key poll.
    session.read_until(b"key?", 40)
    session.write_line("s")
    tail = session.read_until(b"[M-TCMD-OP] swap", 20)
    c.check("TCMD swap op", b"[M-TCMD-OP] swap" in tail,
            tail[-300:].decode("utf-8", "replace"))

    key = "q"
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


def _taskmgr(dev, c):
    """Drive the batch task manager through its own UI: kill one spawned
    victim via the Kill flow, suspend/resume + nice a second victim, save a
    snapshot, Back. Victim slot names come from the `start` reply, never
    assumed (the pool is shared with services). Victims run an endless
    delay loop: `delay` clamps to 10 s, so a plain `start delay N` would
    expire mid-flow and fake every assertion."""
    import re
    dev.push_file("TMGRVICT.BAT",
                  b"@echo off\r\necho [M-TMGR-VICTIM]\r\n:loop\r\ndelay 10000\r\ngoto loop\r\n")
    out = _run(dev, c, "spawn kill victim", "start TMGRVICT.BAT", timeout=20)
    m = re.search(r"\[(bg\d+)\] started", out)
    if m is None:
        c.check("taskmgr victim spawned", False, out[-200:])
        return
    victim = m.group(1)
    out2 = _run(dev, c, "spawn freeze victim", "start TMGRVICT.BAT", timeout=20)
    m2 = re.search(r"\[(bg\d+)\] started", out2)
    victim2 = m2.group(1) if m2 else None
    if victim2 is None:
        c.check("taskmgr second victim spawned", False, out2[-200:])
        dev.run("taskkill %s" % victim)
        dev.run("del /p TMGRVICT.BAT")
        return
    # Both victims must be listed as running their loop file: proves they
    # are really alive (not just claimed) before the UI touches them. (The
    # [M-TMGR-VICTIM] print cannot gate this: it fires during the spawn
    # round-trip and is consumed there.)
    viva = _run(dev, c, "victims listed", "start", timeout=20)
    c.check("victims alive", viva.count("TMGRVICT.BAT") >= 2,
            viva[-200:])
    s = dev.session
    s.reset_input()
    s.write_line("launch TASKMGR")
    head = s.read_until(b"[M-TMGR]", 60)
    c.check("TASKMGR start marker", b"[M-TMGR]" in head,
            head[-300:].decode("utf-8", "replace"))
    s.read_for(4.0)
    _screenshot(dev, c, "apps_taskmgr")

    def choose(entry, marker, label):
        s.read_for(3.0)
        s.write_line(entry)
        tail = s.read_until(marker.encode(), 30)
        c.check(label, marker.encode() in tail,
                tail[-300:].decode("utf-8", "replace"))
        return tail

    # Kill flow: item 2 -> set /p job -> Kill dialog -> ok.
    choose("2", "[M-TMGR-KILL]", "TASKMGR kill flow")
    s.read_for(2.0)
    s.write_line(victim)
    s.read_for(2.0)
    s.write_line("ok")
    s.read_for(2.0)
    # Suspend flow: item 3 -> set /p job -> dialog -> ok. The refresh
    # redraws the pool, so the suspended row proves the freeze.
    choose("3", "[M-TMGR-SUSP]", "TASKMGR suspend flow")
    s.read_for(2.0)
    s.write_line(victim2)
    s.read_for(2.0)
    s.write_line("ok")
    tail = s.read_until(b"suspended", 25)
    c.check("victim suspended in the pool", b"suspended" in tail,
            tail[-300:].decode("utf-8", "replace"))
    # Resume flow: item 4 -> set /p job -> dialog -> ok.
    choose("4", "[M-TMGR-RES]", "TASKMGR resume flow")
    s.read_for(2.0)
    s.write_line(victim2)
    s.read_for(2.0)
    s.write_line("ok")
    s.read_for(2.0)
    # Priority flow: item 5 -> set /p job -> set /p prio -> dialog -> ok.
    # The refresh row carries the new priority.
    choose("5", "[M-TMGR-NICE]", "TASKMGR priority flow")
    s.read_for(2.0)
    s.write_line(victim2)
    s.read_for(2.0)
    s.write_line("3")
    s.read_for(2.0)
    s.write_line("ok")
    tail = s.read_until(b"prio 3", 25)
    c.check("priority applied in the pool", b"prio 3" in tail,
            tail[-300:].decode("utf-8", "replace"))
    # Save flow: item 8 -> snapshot marker -> dialog -> ok.
    choose("8", "[M-TMGR-SAVED]", "TASKMGR save flow")
    s.read_for(2.0)
    s.write_line("ok")
    s.read_for(2.0)
    # Back: item 9 -> quit marker -> shell.
    choose("9", "[M-TMGR-BACK]", "TASKMGR back marker")
    c.check("TASKMGR returned to the shell", _shell_echo(dev), "")
    # Victim 1 is dead (killed through the UI); victim 2 runs at prio 3.
    # Both run the same loop file, so tell them apart by slot: victim 1's
    # slot is idle again, victim 2's still runs it.
    pool = _run(dev, c, "pool after UI kill", "start", timeout=20)
    c.expect("UI kill cleared the victim slot", "%s: idle" % victim, pool)
    c.expect("survivor still listed", "%s: running" % victim2, pool)
    c.expect("machine rows parse", "%s|running" % victim2,
             _run(dev, c, "start /b rows", "start /b", timeout=20))
    snap = _run(dev, c, "snapshot exists", "dir TMGR_SNAP.TXT", timeout=20)
    c.expect("snapshot file written", "TMGR_SNAP.TXT", snap)
    dev.run("taskkill %s" % victim2)
    dev.run("del /p TMGR_SNAP.TXT")
    dev.run("del /p TMGRVICT.BAT")


def _notes(dev, c):
    """Drive New note -> List (db count + pipe listing) -> Back. The store may
    exist from previous runs (first boot only dialogs the create)."""
    s = dev.session
    s.reset_input()
    s.write_line("launch NOTES")
    head = s.read_until(b"[M-NOTE]", 60)
    c.check("NOTES start marker", b"[M-NOTE]" in head,
            head[-300:].decode("utf-8", "replace"))
    # First run creates the store and pops a dialog; dismiss it, then Back.
    s.read_for(12.0)
    s.write_line("ok")
    s.read_for(2.0)
    _screenshot(dev, c, "apps_notes")

    # New note: menu 1 -> ask key -> category 2 (Permanent) -> "Type it".
    s.read_for(4.0)
    s.write_line("1")
    s.read_for(2.5)
    s.write_line("suitek")
    s.read_for(2.0)
    s.write_line("2")
    s.read_for(2.0)
    s.write_line("1")
    s.read_for(2.5)
    s.write_line("note from suite")
    tail = s.read_until(b"[M-NOTE-SAVED]", 40)
    c.check("NOTES saved marker", b"[M-NOTE-SAVED]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    _ok(s)
    # List notes: 2 -> db count + pipe listing -> pause -> Back.
    s.write_line("2")
    tail = s.read_until(b"[M-NOTE-COUNT]", 40)
    c.check("NOTES count marker", b"[M-NOTE-COUNT]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    s.read_for(2.0)
    s.write_line("x")   # pause key after the listing view
    s.read_for(2.0)
    # "Back" is the 8th menu entry; send twice so a slow listing (or a late
    # dialog) that swallowed the first line cannot leave the menu open.
    s.write_line("8")
    s.read_for(2.0)
    s.write_line("8")
    c.check("NOTES returned to the shell", _shell_echo(dev), "")


def _elite(dev, c):
    """Buy one ton of Food (set /a live-name price lookup), save the arrays,
    then quit. Ugly desyncs land in a dock list or ask, both of which swallow
    the numeric sends, so a retry of the whole buy block is safe."""
    s = dev.session
    s.reset_input()
    s.write_line("launch ELITE")
    head = s.read_until(b"[M-ELITE-DOCKED]", 60)
    c.check("ELITE start + dock", b"[M-ELITE]" in head and b"[M-ELITE-DOCKED]" in head,
            head[-300:].decode("utf-8", "replace"))

    bought = False
    tail = b""
    for _ in range(2):
        s.read_for(4.0)
        s.write_line("1")    # Buy
        s.read_for(3.0)
        s.write_line("1")    # Food
        s.read_for(3.0)
        s.write_line("1")    # Tons
        tail = s.read_until(b"[M-ELITE-DOCKED]", 45)
        if b"[M-ELITE-DOCKED]" in tail:
            bought = True
            break
    c.check("ELITE buy round-trip", bought, tail[-300:].decode("utf-8", "replace"))

    tail = _press(s, "4", "[M-ELITE-SAVED]", settle=4.0)  # Save
    c.check("ELITE save marker", b"[M-ELITE-SAVED]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    s.read_for(5.0)   # delay + dock redraw + list
    tail = _press(s, "5", "[M-ELITE-DONE]", settle=4.0)   # Quit
    c.check("ELITE quit marker", b"[M-ELITE-DONE]" in tail,
            tail[-300:].decode("utf-8", "replace"))


def _palm(dev, c):
    """Walk the palmtop-parity entries: calculator (ask value), stopwatch,
    CSV sheet, TCP probe and the crypt note viewer, then Back."""
    s = dev.session
    s.reset_input()
    s.write_line("launch PALMTOP")
    head = s.read_until(b"[M-PALM]", 60)
    c.check("PALMTOP start marker", b"[M-PALM]" in head,
            head[-300:].decode("utf-8", "replace"))
    _screenshot(dev, c, "apps_palmtop")

    s.read_for(4.0)
    s.write_line("1")    # Calculator
    s.read_for(3.0)
    s.write_line("42")   # ask value
    tail = s.read_until(b"[M-PALM-CALC]", 45)
    c.check("PALMTOP calc marker", b"[M-PALM-CALC]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    s.read_for(2.0)
    s.write_line("x")    # pause key
    s.read_for(3.0)

    for sel, marker in (("2", "[M-PALM-TIMER]"), ("3", "[M-PALM-CSV]"),
                        ("6", "[M-PALM-TCP]")):
        tail = _press(s, sel, marker, settle=4.0, timeout=60.0)
        c.check("PALMTOP %s" % marker, marker.encode() in tail,
                tail[-300:].decode("utf-8", "replace"))
        s.read_for(2.0)
        s.write_line("x")  # pause key
        s.read_for(3.0)

    # Encrypt a note: the marker precedes the note viewer (q closes it).
    tail = _press(s, "4", "[M-PALM-CRYPT]", settle=4.0, timeout=45.0)
    c.check("PALMTOP crypt marker", b"[M-PALM-CRYPT]" in tail,
            tail[-300:].decode("utf-8", "replace"))
    s.read_for(3.0)
    # Close the note viewer; retry in case the first q raced the modal opening.
    viewer_closed = False
    for _ in range(4):
        if _shell_echo(dev):
            viewer_closed = True
            break
        s.read_for(1.0)
        s.write_line("q")
        s.read_for(2.0)
    c.check("PALMTOP note viewer closed", viewer_closed, "")

    s.read_for(2.0)
    # Back to the shell. The sub-app may be draining output when the first
    # `7` lands, so retry until the shell actually answers (an open `list`
    # ignores a stray line and keeps the app on its menu).
    returned = viewer_closed
    for _ in range(5):
        if returned:
            break
        s.write_line("7")
        s.read_for(1.5)
        if _shell_echo(dev):
            returned = True
            break
    c.check("PALMTOP returned to the shell", returned, "")


def _diag(dev, c):
    """Run the Identity section and the 150-frame gfx Benchmark (the `while n
    LSS 150 do call :bench_frame` loop), then Exit."""
    s = dev.session
    s.reset_input()
    s.write_line("launch DIAG")
    head = s.read_until(b"[M-DIAG]", 60)
    c.check("DIAG start marker", b"[M-DIAG]" in head,
            head[-300:].decode("utf-8", "replace"))
    c.check("DIAG menu marker", _wait(s, "[M-DIAG-MENU]", 45), "")

    tail = _press(s, "2", "[M-DIAG-MENU]", settle=4.0, timeout=90.0)  # Identity
    c.check("DIAG identity ran", b"[M-DIAG-SECTION] identity" in tail,
            tail[-300:].decode("utf-8", "replace"))

    tail = _press(s, "7", "[M-DIAG-MENU]", settle=4.0, timeout=180.0)  # Benchmark
    c.check("DIAG bench ran", b"[M-DIAG-SECTION] benchmark" in tail,
            tail[-300:].decode("utf-8", "replace"))
    c.check("DIAG bench frame count", b"frame pacing (150 frames)" in tail,
            tail[-300:].decode("utf-8", "replace"))

    tail = _press(s, "9", "[M-DIAG-DONE]", settle=4.0, timeout=60.0)   # Exit
    c.check("DIAG done marker", b"[M-DIAG-DONE]" in tail,
            tail[-300:].decode("utf-8", "replace"))


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


def _companion_deep(dev, c):
    """Port of apps/companion/deep_test.py minus the per-section board resets:
    the menu walk across all five sub-apps, then each sub-app driven stand-
    alone through its new-feature paths (Services/Packages, Note pad, fetch,
    calc, Theme/Rotate/Quick settings)."""
    s = dev.session

    # ---- COMPANION main menu walk (each sub entered and q-cancelled) ----
    mods = [("1", "[M-SYS]", "[M-SYS-BACK]"),
            ("2", "[M-FILES]", "[M-FILES-BACK]"),
            ("3", "[M-NET]", "[M-NET-BACK]"),
            ("4", "[M-FUN]", "[M-FUN-BACK]"),
            ("5", "[M-SET]", "[M-SET-BACK]")]
    s.reset_input()
    s.write_line("COMPANION.BAT")
    walk = True
    got = set()
    for sel, mark, backmark in mods:
        if not _wait(s, "[C-MENU]", 50):
            walk = False
            break
        tail = _press(s, sel, mark, settle=3.0)
        got.add(mark)
        if backmark.encode() not in tail:
            tail = _press(s, "q", backmark, settle=3.0)
        if backmark.encode() not in tail:
            walk = False
            break
    if walk and _wait(s, "[C-MENU]", 50):
        tail = _press(s, "6", "[C-EXIT]", settle=3.0)
        walk = b"[C-EXIT]" in tail
    c.check("companion menu walk", walk and len(got) == 5, "mods=%s" % sorted(got))

    # ---- SYS deep: Save, Services, Packages ----
    s.reset_input()
    s.write_line("call SYS.BAT::main")
    sys_ok = _wait(s, "[M-SYS]", 40)
    if sys_ok:
        tail = _press(s, "3", "[M-SYS-SAVED]", settle=4.0)   # Save snapshot
        sys_ok = b"[M-SYS-SAVED]" in tail
        if sys_ok:
            _ok(s)
            tail = _press(s, "4", "[M-SYS-SVC]", settle=4.0)  # Services
            sys_ok = b"[M-SYS-SVC]" in tail
            if sys_ok:
                tail = _press(s, "1", "[M-SVC-STATUS]", settle=3.0)
                sys_ok = b"[M-SVC-STATUS]" in tail
                if sys_ok:
                    _ok(s)
                    s.read_for(4.0)
                    s.write_line("6")   # services Back -> dashboard
                    s.read_for(3.0)
                    tail = _press(s, "5", "[M-SYS-PKG]", settle=3.0)  # Packages
                    sys_ok = b"[M-SYS-PKG]" in tail
                    if sys_ok:
                        s.read_for(4.0)
                        s.write_line("5")   # packages Back -> dashboard
                        s.read_for(3.0)
                    tail = _press(s, "6", "[M-SYS-BACK]", settle=3.0)  # Back
                    sys_ok = b"[M-SYS-BACK]" in tail
    c.check("SYS deep (save/services/packages)", sys_ok, "")
    if sys_ok:
        out = _run(dev, c, "sys snapshot", "type SYS_SNAP.TXT", timeout=15)
        c.expect("SYS snapshot holds mem", "mem.heap", out)

    # ---- FILES deep: Note pad -> new note -> note Back (exits FILES) ----
    s.reset_input()
    s.write_line("call FILES.BAT::main")
    files_ok = _wait(s, "[M-FILES]", 40)
    if files_ok:
        tail = _press(s, "6", "[M-FILES-NOTES]", settle=4.0)   # Note pad
        files_ok = b"[M-FILES-NOTES]" in tail
        if files_ok:
            s.read_for(4.0)
            s.write_line("1")           # New note -> ask name
            s.read_for(3.0)
            s.write_line("deepnote")
            s.read_for(3.0)
            s.write_line("deep body")
            tail = s.read_until(b"[M-FILES-NOTED]", 40)
            files_ok = b"[M-FILES-NOTED]" in tail
            if files_ok:
                _ok(s)
                s.read_for(4.0)
                s.write_line("5")       # notes Back -> :back (exits FILES)
                tail = s.read_until(b"[M-FILES-BACK]", 30)
                files_ok = b"[M-FILES-BACK]" in tail
    c.check("FILES deep (note pad)", files_ok, "")
    if files_ok:
        out = _run(dev, c, "files note artifact", "type NOTES/deepnote.txt", timeout=15)
        c.expect("FILES note holds body", "deep body", out)

    # ---- NET deep (offline fetch degrades cleanly) ----
    s.reset_input()
    s.write_line("call NET.BAT::main")
    net_ok = _wait(s, "[M-NET]", 40)
    if net_ok:
        tail = _press(s, "6", "[M-NET-FETCHED]", settle=4.0, timeout=90.0)  # Fetch
        net_ok = b"[M-NET-FETCHED]" in tail
        if net_ok:
            _ok(s)
            s.read_for(3.0)
            s.write_line("q")           # close viewer (or cancel/unknown line)
            s.read_for(3.0)
            tail = _press(s, "8", "[M-NET-BACK]", settle=3.0)  # Back
            # A q-cancelled main menu may have already emitted the backmark.
            net_ok = b"[M-NET-BACK]" in tail
    c.check("NET deep (fetch)", net_ok, "")

    # ---- FUN deep: calc playground -> Result viewer ----
    s.reset_input()
    s.write_line("call FUN.BAT::main")
    fun_ok = _wait(s, "[M-FUN]", 40)
    if fun_ok:
        s.read_for(4.0)
        s.write_line("4")               # Calc playground -> ask expr
        s.read_for(3.0)
        s.write_line("6*7")
        s.read_for(5.0)                 # calc runs, Result viewer opens
        s.write_line("q")               # close viewer
        s.read_for(3.0)
        tail = _press(s, "9", "[M-FUN-BACK]", settle=3.0)  # Back
        fun_ok = b"[M-FUN-BACK]" in tail
    c.check("FUN deep (calc)", fun_ok, "")
    if fun_ok:
        out = _run(dev, c, "calc check", "calc 6*7", timeout=15)
        c.expect("calc 6*7 == 42", "42", out)

    # ---- SET deep: brightness, theme, rotate round-trip, quick form ----
    s.reset_input()
    s.write_line("call SET.BAT::main")
    set_ok = _wait(s, "[M-SET]", 40)
    set_stage = "main menu"
    if set_ok:
        def _advance(marker, tries=3):
            """Dismiss a pending dialog with `ok` until `marker` shows. The
            waits keep their leftovers, so the marker emitted after a
            dismissal is not lost."""
            for _ in range(tries):
                if _wait(s, marker, 12):
                    return True
                s.write_line("ok")
            return _wait(s, marker, 12)

        s.read_for(4.0)
        set_stage = "brightness"
        s.write_line("1")               # Set brightness -> ask value
        s.read_for(3.0)
        s.write_line("63")
        s.read_for(4.0)
        set_ok = _advance("[M-SET]")    # dismiss Saved dialog -> Settings menu
        if set_ok:
            # Theme: enter, set amber, dismiss, Theme Back -> Settings.
            set_stage = "theme open"
            tail = _press(s, "6", "[M-SET-THEME]", settle=3.0)
            set_ok = b"[M-SET-THEME]" in tail
            if set_ok:
                set_stage = "theme saved"
                tail = _press(s, "2", "[M-SET-THEME-SAVED]", settle=3.0)
                set_ok = b"[M-SET-THEME-SAVED]" in tail
                if set_ok:
                    set_stage = "theme dismiss"
                    s.write_line("ok")
                    set_ok = _advance("[M-SET-THEME]")
                    if set_ok:
                        set_stage = "theme back"
                        tail = _press(s, "6", "[M-SET]", settle=3.0)  # Theme Back
                        set_ok = b"[M-SET]" in tail
    c.check("SET deep (brightness/theme)", set_ok, "" if set_ok else set_stage)
    if set_ok:
        # Leave SET before running shell commands (the menu owns the worker).
        set_stage = "exit"
        tail = _press(s, "13", "[M-SET-BACK]", settle=3.0)
        set_ok = b"[M-SET-BACK]" in tail
        c.check("SET deep exit", set_ok, "" if set_ok else set_stage)
    if set_ok:
        out = _run(dev, c, "set brightness", "config BRIGHTNESS /b", timeout=15)
        c.expect("brightness applied 63", "63", out)

    # Rotate submenu navigation. An *applied* rotation rebuilds the UI and
    # non-deterministically closes the foreground TUI app, so the submenu is
    # exercised for navigation here and the rotation itself is driven from the
    # shell below (deterministic, no app owning the screen).
    rot_ok = True
    s.reset_input()
    s.write_line("call SET.BAT::main")
    if not _wait(s, "[M-SET]", 40):
        rot_ok = False
    else:
        s.read_for(4.0)
        tail = _press(s, "3", "[M-SET-ROTATE]", settle=3.0)
        rot_ok = b"[M-SET-ROTATE]" in tail
        if rot_ok:
            tail = _press(s, "5", "[M-SET]", settle=3.0)  # Back
            rot_ok = b"[M-SET]" in tail
        if rot_ok:
            tail = _press(s, "13", "[M-SET-BACK]", settle=3.0)  # exit
            rot_ok = b"[M-SET-BACK]" in tail
    c.check("SET rotate submenu", rot_ok, "")

    # Rotation persistence round-trip (90 then back to 0).
    for value in ("90", "0"):
        _run(dev, c, "config ROTATE", "config ROTATE %s" % value, timeout=20)
        out = _run(dev, c, "config ROTATE /b", "config ROTATE /b", timeout=15)
        c.expect("rotation persisted %s" % value, value, out)

    # Quick settings form: accept the prefilled values and verify they applied.
    # Accepting runs `config HEADER ON`, which schedules a UI rebuild that by
    # design closes the foreground TUI app, so wait for the shell rather than
    # for a SET marker.
    def _shell_alive(timeout=6):
        try:
            return "P4QOK" in dev.run("echo P4QOK", timeout=timeout)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            return False

    s.reset_input()
    s.write_line("call SET.BAT::main")
    quick_ok = _wait(s, "[M-SET]", 40)
    if quick_ok:
        s.read_for(4.0)
        tail = _press(s, "14", "[M-SET-QUICK]", settle=3.0)
        quick_ok = b"[M-SET-QUICK]" in tail
        if quick_ok:
            s.read_for(2.0)
            s.write_line("ok")          # accept the prefilled form
            quick_ok = False
            for _ in range(10):
                s.read_for(2.0)
                if _shell_alive():
                    quick_ok = True
                    break
                s.write_line("13")
    c.check("SET quick settings form", quick_ok, "")
    if quick_ok:
        out = _run(dev, c, "quick brightness", "config BRIGHTNESS /b", timeout=15)
        c.expect("quick brightness 50", "50", out)
        out = _run(dev, c, "quick volume", "config VOLUME /b", timeout=15)
        c.expect("quick volume 50", "50", out)


def _cleanup(dev, c):
    artifacts = [
        "BOUNCE.BMP", "SNAKE.BMP", "GFXTOOL.BMP", "PLOT.BMP", "TCMD.BMP",
        "DIAG_BENCH.BMP", "ELITE.BMP", "DIAG.TXT", "ADVENT.SAV", "ELITE.SAV",
        "_z.txt", "_zinit.txt", "_m.txt", "_s.txt", "_tz.txt", "_d.txt",
        "_mood.txt", "_LT.txt", "_RT.txt", "_palm.csv",
        "SYS_SNAP.TXT", "funurl.txt", "_joke.txt", "_known.txt", "_net.txt",
        "NOTES/deepnote.txt", "NOTES/suitek.txt",
    ]
    for name in artifacts:
        try:
            dev.run("del %s" % name, timeout=15)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    for cmd in ("draw close", "taskkill bg0", "gfx close", "config ROTATE 0"):
        try:
            dev.run(cmd, timeout=15)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    c.note("cleanup complete; deployed app payloads left installed")


def _services(dev, c):
    """Resident services end to end: install/start the MOOD sampler and the
    MEMLOG logger, prove ticks/files/events/supervision, then stop + remove
    everything (no TSRs left behind) and delete the registry + artifacts."""
    out = dev.run('service install P14MOOD "MOODSVC" /restart /noauto')
    c.expect("service installs the sampler", "installed", out)
    out = dev.run("service start P14MOOD")
    c.expect("service starts the sampler", "started", out)
    head = dev.run("delay 12000", timeout=60)
    c.expect("sampler ticked", "[M-MOODSVC]", head)
    out = dev.run("type sd:/MOODLAST.INI")
    c.expect("sampler published heap_free", "heap_free=", out)
    out = dev.run("service list")
    c.expect("sampler supervised running", "running", out)
    dev.run("service stop P14MOOD")
    dev.run("service remove P14MOOD")
    out = dev.run('service install P14MEM "MEMLOG 2000" /restart /noauto')
    c.expect("service installs the logger", "installed", out)
    dev.run("service start P14MEM")
    c.expect("logger signalled a tick", "P14WAIT-EL=0",
             dev.run("wait memlogtick /t:10 & echo P14WAIT-EL=%ERRORLEVEL%", timeout=60))
    out = dev.run("type sd:/MEMLOG.CSV")
    c.expect("logger wrote the CSV", "heap_free", out)
    dev.run("service stop P14MEM")
    dev.run("service remove P14MEM")
    dev.run("del /p sd:/MEMLOG.CSV")
    dev.run("del /p sd:/MEMLOG.OLD")
    dev.run("del /p sd:/MOODLAST.INI")
    dev.run("del /p sd:/_memlog.txt")
    dev.run("del /p sd:/_moods.txt")
    dev.run("del /p SERVICES.INI")


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

    for name, start, done, timeout, extra in NONSTOP:
        _oneshot(dev, c, name, start, done, timeout, extra)

    # Hybrid model: every app launches through a .BAT shim, even when a
    # linked-in C entry does the work (see ABI.md).
    c.expect("launch tags hybrid shims", "[hybrid]", listed)
    hybrid = _run(dev, c, "launch HYBRID", "launch HYBRID", timeout=60.0)
    c.expect("hybrid shim reaches native entry", "HELLO_RESULT=ok", hybrid)
    c.expect("hybrid shim sees no BAD markers", "[M-HYBRID] BAD", hybrid, want=False)

    _lib(dev, c)
    _advent(dev, c)
    _snake(dev, c)
    _mood(dev, c)
    _tcmd(dev, c)
    _taskmgr(dev, c)
    _notes(dev, c)
    _elite(dev, c)
    _drive(dev, c, *DRIVE[0])    # PICS
    _palm(dev, c)
    _diag(dev, c)
    _drive(dev, c, *DRIVE[1])    # COMPANION smoke + screenshot
    _companion_deep(dev, c)
    _services(dev, c)

    if quick:
        c.note("quick mode: no extra long-app interaction")

    _cleanup(dev, c)
    return c