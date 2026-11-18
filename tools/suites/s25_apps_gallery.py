# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""App gallery: drive every reference app to its main screen and publish PNGs.

This is the v1.4.3 docs pipeline. It deploys the whole reference bundle from
``apps/app_manifest.py`` (entry ``.BAT`` files, ``APPS/*.APPINFO`` metadata,
the browser site, the emoji pack, the local ``DOCS/`` pages and the sprite
assets), then runs each app deterministically to a representative screen and
writes ``docs/assets/apps/<app>.png`` at the native panel size.

Two capture paths, both going through :mod:`p4test.screenshot`:

* **streaming** (``p4test.screenshot.capture`` with ``unwind=False``). The
  bare ``screenshot`` token is claimed by the console reader while a native
  modal blocks the command worker (menu ``list``, input ``ask``, the image
  viewer), so this is how every menu/app prompt is captured. No Ctrl-C is
  ever sent: the app under test must survive the shot.
* **app-written BMP** (``parse_bmp`` on a file the app saved itself) for the
  three surfaces a host capture cannot reach: TCMD and SNAKE sit in a
  ``choice`` key wait that owns the console input, and the canvas apps
  (BOUNCE/GFXTOOL/PLOT) run a no-I/O frame loop that would queue the
  command until after the canvas closed. Those apps already write their own
  BMPs (documented keys ``p`` for TCMD/SNAKE, the end-of-run save for the
  canvas apps); the suite pulls that file and feeds it through the same
  :class:`~p4test.screenshot.Bmp` pixel model.

Each app gets exactly one gallery gate: the PNG opens at the native panel
size and is not blank. A blank screen (uniform pixels) is the failure this
pipeline exists to catch: an app that drew nothing, a display that slept, or
a capture of a shell that had already replaced the app.

The docs folder is 1024x600 by convention (the reference board). On a
different panel the check fails with the measured size instead of publishing
a wrong-size PNG, so a Tab5 run cannot silently overwrite the docs set.

Everything is idempotent: the payloads stay installed (like ``push_apps.py``)
and only per-run artifacts (snapshots, temp CSVs, service logs, pulled BMPs)
are removed.
"""
from __future__ import annotations

import io
import os
import re
import sys
import time

from p4test.asserts import Checklist
from p4test.session import PanicError
from p4test.screenshot import parse_bmp

NAME = "apps_gallery"
TAGS = ["apps", "gallery", "slow"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
DOCS = os.path.join(ROOT, "docs", "assets", "apps")
REFERENCE_SIZE = (1024, 600)

# Apps whose PNG was written by the current run (the final gate uses this so a
# stale file from an older run can never stand in for a failed capture).
WRITTEN = set()

sys.path.insert(0, APPS)

from app_manifest import (  # noqa: E402
    APP_PAYLOADS,
    COMPANION_PAYLOADS,
    payload_path,
    remote_target,
)

# ---------------------------------------------------------------------------
# Driver table
#
# ``cmd``       command that starts the app / surface
# ``start``     marker that proves the app is on its main screen
# ``pre``       (delay, line, wait_marker|None) steps before the shot
# ``shot``      "stream" | ("file", remote) | ("snap", key, marker, remote)
# ``quit``      lines that take the app back to the shell
# ``end``       marker proving the clean exit (None = shell echo is enough)
# ``end_shell`` require a real shell answer after the quit lines
# ``done``      for one-shots: the marker the app emits before we shoot
# ---------------------------------------------------------------------------
SPECS = [
    # ---- TUI shells and menus -----------------------------------------
    dict(app="tcmd", cmd="launch TCMD", start="[M-TCMD-R]",
         shot=("snap", "p", "[M-TCMD-R]", "TCMD.BMP"),
         quit=["q"], end="[M-TCMD-DONE]"),
    dict(app="diag", cmd="launch DIAG", start="[M-DIAG-MENU]",
         shot="stream", shot_delay=1.0, quit=["9"], end="[M-DIAG-DONE]"),
    dict(app="palmtop", cmd="launch PALMTOP", start="[M-PALM]",
         shot="stream", shot_delay=3.0, quit=["7"], end_shell=True),
    dict(app="notes", cmd="launch NOTES", start="[M-NOTE]",
         pre=[(2.0, "ok", None)], shot="stream", shot_delay=1.5,
         quit=["8"], end_shell=True),
    dict(app="companion", cmd="launch COMPANION", start="[C-MENU]",
         shot="stream", shot_delay=2.0, quit=["6"], end="[C-EXIT]"),
    dict(app="sys", cmd="launch SYS", start="[M-SYS]",
         shot="stream", shot_delay=3.0, quit=["6"], end="[M-SYS-BACK]"),
    dict(app="files", cmd="launch FILES", start="[M-FILES]",
         shot="stream", shot_delay=3.0, quit=["9"], end="[M-FILES-BACK]"),
    dict(app="net", cmd="launch NET", start="[M-NET]",
         shot="stream", shot_delay=3.0, quit=["8"], end="[M-NET-BACK]"),
    dict(app="fun", cmd="launch FUN", start="[M-FUN]",
         shot="stream", shot_delay=3.0, quit=["9"], end="[M-FUN-BACK]"),
    dict(app="set", cmd="launch SET", start="[M-SET]",
         shot="stream", shot_delay=3.0, quit=["13"], end="[M-SET-BACK]"),
    dict(app="chat", cmd="launch CHAT", start="[M-CHAT]",
         shot="stream", shot_delay=3.0, quit=["9"], end="[M-CHAT-DONE]"),
    dict(app="taskmgr", cmd="launch TASKMGR", start="[M-TMGR]",
         shot="stream", shot_delay=4.0, quit=["9"], end="[M-TMGR-BACK]"),
    dict(app="elite", cmd="launch ELITE", start="[M-ELITE-DOCKED]",
         shot="stream", shot_delay=2.0, quit=["5"], end="[M-ELITE-DONE]"),

    # ---- pictures: the documented first entry is the full-color viewer --
    dict(app="pics", cmd="launch PICS", start="[M-PICS]",
         pre=[(2.0, "1", "[M-PICS-VIEW]")], shot="stream", shot_delay=1.5,
         quit=["q", "5"], end_shell=True),

    # ---- games / adventures: a few moves so the shot looks alive -------
    dict(app="adventure", cmd="launch ADVENT", start="[M-ADV]",
         pre=[(1.0, "n", "---"), (0.3, "w", "---"),
              (0.3, "snap", "[M-ADV-SNAP]")],
         shot=("file", "ADVENT.BMP"), quit=["quit"], end="[M-ADV-QUIT]"),
    dict(app="snake", cmd="launch SNAKE", start="[M-SNAKE]",
         pre=[(1.2, "s", None)],
         shot=("snap", "p", "[M-SNAKE-SNAP]", "SNAKE.BMP"),
         quit=["q"], end="[M-SNAKE-DONE]"),
    dict(app="mood", cmd="launch MOOD", start="[M-MOOD]",
         pre=[(1.0, "1", "[M-MOOD-TREND]")],
         shot=("file", "MOOD_TREND.BMP"), quit=["ok", "6", "6"],
         quit_settle=1.5, end_shell=True),

    # ---- canvas apps: use the BMP the app saves off its frame loop ------
    dict(app="bounce", cmd="launch BOUNCE", start="[M-BOUNCE]",
         done="[M-BOUNCE-DONE]", shot=("file", "BOUNCE.BMP")),
    dict(app="gfxtool", cmd="launch GFXTOOL", start="[M-GFXTOOL]",
         done="[M-GFXTOOL-DONE]", shot=("file", "GFXTOOL_SCREEN.BMP")),
    dict(app="plot", cmd="launch PLOT", start="[M-PLOT]",
         done="[M-PLOT-DONE]", shot=("file", "PLOT_SCREEN.BMP")),

    # ---- browser showcase: the app captures its own clean boot state ---
    dict(app="web", cmd="launch WEB", start="[M-WEB-LOADED]",
         done="[M-WEB-DONE]", shot=("file", "WEB/SHOT.BMP")),

    # ---- one-shot transcripts ------------------------------------------
    dict(app="writer", cmd="launch WRITER", start="[M-WRITER]",
         done="[M-WRITER-DONE]", shot="stream"),
    dict(app="agenda", cmd="launch AGENDA", start="[M-AGENDA]",
         done="[M-AGENDA-DONE]", shot="stream", shot_delay=1.0),
    dict(app="control", cmd="launch CONTROL", start="[M-CONTROL]",
         done="[M-CONTROL-DONE]", shot="stream"),
    dict(app="hybrid", cmd="launch HYBRID", start="[M-HYBRID]",
         done="[M-HYBRID-DONE]", shot="stream"),
    dict(app="pkgtest", cmd="launch PKGTEST", start="[M-PKGTEST]",
         done="[M-PKGTEST-DONE]", shot="stream"),
    dict(app="uitest", cmd="launch UITEST", start="[M-UITEST]",
         done="[M-UITEST-DONE]", shot="stream"),
    dict(app="lib", cmd="call LIB.BAT::selftest_all", start="SELFTEST done",
         shot="stream"),
    dict(app="appl", cmd="call APPLIB.BAT::emoji_demo", start="[M-FEAT-EMOJI]",
         shot="stream"),
    dict(app="aliases", cmd="alias /load ALIASES.BAT & alias", start=None,
         shot="stream", shot_delay=1.0),

    # ---- extra sub-screens that document a feature in place ------------
    dict(app="files-notes", cmd="launch FILES", start="[M-FILES]",
         pre=[(4.0, "6", "[M-FILES-NOTES]")], shot="stream", shot_delay=1.0,
         quit=["5"], end="[M-FILES-BACK]"),
    dict(app="net-msg", cmd="launch NET", start="[M-NET]",
         pre=[(4.0, "7", "[M-NET-OUTBOX]")], shot="stream", shot_delay=1.0,
         quit=["1", "8"], quit_settle=2.0, end="[M-NET-BACK]"),
]

# Background-service surfaces: start the real script in a pool slot, wait for
# its first tick, capture the transcript, then kill the slot.
BG_SPECS = [
    dict(app="svc", cmd="start SVC.BAT", start="[M-SVC-TICK]", quit_marker="[M-SVC]"),
    dict(app="memlog", cmd="start MEMLOG.BAT 5000",
         start="[M-MEMLOG-SAMPLE]", quit_marker="[M-MEMLOG]"),
    dict(app="moodsvc", cmd="start MOODSVC.BAT", start="[M-MOODSVC]",
         quit_marker="[M-MOODSVC]"),
]

TEMP_FILES = [
    "BOUNCE.BMP", "SNAKE.BMP", "GFXTOOL.BMP", "PLOT.BMP", "TCMD.BMP",
    "GFXTOOL_SCREEN.BMP", "PLOT_SCREEN.BMP",
    "DIAG_BENCH.BMP", "ELITE.BMP", "DIAG.TXT", "ADVENT.SAV", "ELITE.SAV",
    "ADVENT.BMP", "MOOD_TREND.BMP",
    "_mood.txt", "_m.txt", "_s.txt", "_tz.txt", "_d.txt", "_palm.csv",
    "SYS_SNAP.TXT", "TMGR_SNAP.TXT", "WRITERDEMO.MD", "WRITERDEMO.HTML",
    "WRITERDEMO.TXT", "WRITERDEMO.PRN", "WEB/SHOT.BMP",
    "_memlog.txt", "MEMLOG.CSV", "MEMLOG.OLD", "_moods.txt", "MOODLAST.INI",
    "funurl.txt", "_joke.txt", "_calc.txt", "_theme.txt", "_font.txt",
    "_set.txt", "_dir.txt", "_search.txt", "_trash.txt", "_clip.txt",
    "_notes.txt", "_net.txt", "_known.txt", "_ping.txt", "_dns.txt",
    "_p4parity_h.txt", "_p4parity_in.txt", "_p4parity_eol.txt",
    "_palm_note.txt", "_palm_note.enc", "_palm_note.out", "_palm_view.txt",
]


def _tail(data) -> str:
    if isinstance(data, bytes):
        data = data.decode("utf-8", "replace")
    return data[-400:].replace("\r", "")


def _log(text):
    """Progress line: the runner buffers stdout, so be explicit."""
    print("  [gallery] %s" % text, flush=True)


def _run(dev, c, label, cmd, timeout=60.0):
    try:
        return dev.run(cmd, timeout=timeout)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("%s: %r did not return: %s" % (label, cmd, exc))
        return ""


def _push(dev, c, local, remote):
    try:
        dev.push_local(local, remote)
        return True
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("push %s" % remote, False, str(exc))
        return False

def _make_sprites(tmp):
    """The same deterministic BALL/PHOTO art as apps/push_assets.py."""
    from PIL import Image, ImageDraw

    ball = os.path.join(tmp, "BALL.BMP")
    im = Image.new("RGB", (16, 16), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([1, 1, 14, 14], fill=(255, 255, 255))
    d.ellipse([5, 5, 10, 10], fill=(255, 0, 0))
    im.save(ball)

    photo = os.path.join(tmp, "PHOTO.BMP")
    im = Image.new("RGB", (96, 64))
    px = im.load()
    for y in range(64):
        for x in range(96):
            px[x, y] = (int(20 + (y / 63.0) * 200),
                        int(60 + (y / 63.0) * 120),
                        int(160 - (y / 63.0) * 120))
    d = ImageDraw.Draw(im)
    d.ellipse([64, 8, 84, 28], fill=(255, 230, 120))
    d.polygon([(0, 64), (30, 30), (60, 64)], fill=(30, 110, 45))
    d.polygon([(40, 64), (72, 36), (96, 64)], fill=(20, 80, 35))
    im.save(photo)
    return [(ball, "BALL.BMP"), (photo, "PHOTO.BMP")]


def _deploy(dev, c, tmp):
    _log("deploying the reference bundle")
    for name in ("APPS", "WEB", "EMOJI", "DOCS", "TEMPLATES", "DICTS",
                 "NOTES", "tmp"):
        _run(dev, c, "mkdir %s" % name, "md %s" % name, timeout=20)
    payloads = APP_PAYLOADS + COMPANION_PAYLOADS
    for index, (subdir, name, target) in enumerate(payloads):
        _push(dev, c, payload_path(subdir, name), remote_target(name, target))
        if index % 12 == 0:
            _log("pushed %d/%d payloads" % (index, len(payloads)))
    for local, remote in _make_sprites(tmp):
        _push(dev, c, local, remote)
    # WRITER needs both template seeds and the sample wordlist.
    for name in ("NOTE.MD", "LETTER.MD", "LOG.MD"):
        _push(dev, c, os.path.join(APPS, "templates", name),
              "TEMPLATES/" + name)
    _push(dev, c, os.path.join(APPS, "dicts", "en.words"), "DICTS/en.words")
    _log("deployment complete")
    listed = _run(dev, c, "launch /list", "launch /list", timeout=30)
    c.expect("gallery payloads are installed", "TCMD", listed)
    c.expect("gallery metadata is installed", "Commander", listed)


def _shell_echo(dev, tag="P4GAL"):
    try:
        return tag in dev.run("echo " + tag, timeout=25)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        return False


def _ensure_shell(dev, c, app, extra_quit=()):
    """Return True only when the SHELL answers; recover a wedged console.

    The large ``send`` frames used by the app-written BMP path can leave the
    USB-Serial-JTAG console briefly unresponsive (observed after a 2.7 MB
    pull). A foreground break first; if that is not enough, a hard reset.
    The SD payload is untouched, so the next app always starts clean.
    """
    if _shell_echo(dev):
        return True
    for _ in range(3):
        for line in extra_quit:
            try:
                dev.session.write_line(line)
            except Exception:  # noqa: BLE001
                pass
        try:
            dev.session.write(b"\x03\r\n")
        except Exception:  # noqa: BLE001
            pass
        dev.session.read_for(1.0)
        if _shell_echo(dev):
            c.note("%s: recovered the shell with a foreground break" % app)
            return True
    try:
        dev.reboot(settle_ms=1500)
        if _shell_echo(dev):
            c.note("%s: recovered the shell with a hard reset" % app)
            return True
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass
    return False


def _image_from_bmp(bmp):
    from PIL import Image
    img = Image.open(io.BytesIO(bmp.data))
    img.load()
    return img.convert("RGB")


def _ink(bmp):
    """Sampled ink fraction + distinct colours (the blank-screen gate)."""
    from collections import Counter
    counts = Counter()
    for y in range(0, bmp.height, 8):
        for x in range(0, bmp.width, 8):
            counts[bmp.px(x, y)] += 1
    total = sum(counts.values())
    if total == 0:
        return 0.0, 0
    mode = counts.most_common(1)[0][1]
    return 1.0 - (mode / float(total)), len(counts)


def _publish(dev, c, app, bmp, want, source):
    """Write the PNG and run the one-per-app gallery gate."""
    check_name = ("%s: PNG is a valid %dx%d image and not blank"
                  % (app, want[0], want[1]))
    if bmp is None:
        c.check(check_name, False, "no capture from %s" % (source or "the app"))
        return False

    path = os.path.join(DOCS, app + ".png")
    try:
        img = _image_from_bmp(bmp)
    except Exception as exc:  # noqa: BLE001
        c.check("%s: PNG is a valid %dx%d image and not blank"
                % (app, want[0], want[1]), False,
                "%s BMP did not decode: %s" % (source, exc))
        return False

    size_ok = (img.width, img.height) == want
    png_ok = False
    if size_ok and want == REFERENCE_SIZE:
        os.makedirs(DOCS, exist_ok=True)
        try:
            img.save(path, "PNG", optimize=True)
            from PIL import Image
            with Image.open(path) as probe:
                probe.load()
                png_ok = probe.size == want
        except Exception as exc:  # noqa: BLE001
            c.check("%s: PNG is a valid %dx%d image and not blank"
                    % (app, want[0], want[1]), False,
                    "could not write %s: %s" % (path, exc))
            return False

    ink, colours = _ink(bmp)
    # A TUI screen is deliberately low-colour (TCMD/BOUNCE are 4-5 colours:
    # background, ink, accent, sprite). A blank capture has ONE colour, so
    # require a second/third tone plus real ink, not a palette count that
    # would reject a legitimate monochrome app.
    not_blank = ink >= 0.01 and colours >= 3
    ok = size_ok and png_ok and not_blank
    if not size_ok:
        detail = ("captured %dx%d from %s; the docs gallery requires the %dx%d "
                  "reference panel" % (img.width, img.height, source,
                                       want[0], want[1]))
    elif not png_ok:
        detail = "PNG was not written/read back at %dx%d" % want
    elif not not_blank:
        detail = ("blank-looking screen (%.2f%% ink, %d sampled colours) from %s"
                  % (ink * 100.0, colours, source))
    else:
        detail = ""
    c.check(check_name, ok, detail)
    if ok:
        WRITTEN.add(app)
        c.note("%s: %dx%d, %.1f%% ink, %d colours" %
               (app, img.width, img.height, ink * 100.0, colours))
    return ok


def _pull_bmp(dev, c, remote):
    """Pull an app-written BMP and parse it through the shared pixel model."""
    try:
        data = dev.pull_file(remote)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("pull %s failed: %s" % (remote, exc))
        return None
    try:
        return parse_bmp(data)
    except Exception as exc:  # noqa: BLE001
        c.note("parse %s failed: %s" % (remote, exc))
        return None


def _read_until(session, marker, timeout=45.0):
    try:
        return session.read_until(marker.encode(), timeout)
    except Exception:  # noqa: BLE001 - a missing marker is reported by the caller
        return b""


def _drive(dev, c, spec, want):
    app = spec["app"]
    s = dev.session

    # Delete the app's own snapshot first so a stale file can never be taken
    # for this run's capture.
    shot = spec.get("shot")
    if isinstance(shot, tuple) and shot[0] in ("file", "snap"):
        _run(dev, c, app + " clear snapshot", "del /f %s" % shot[-1],
             timeout=20)

    start = spec.get("start")
    if start is None:
        _run(dev, c, app + " surface", spec["cmd"], timeout=60)
    else:
        s.reset_input()
        s.write_line(spec["cmd"])
        head = _read_until(s, start, spec.get("timeout", 60.0))
        if start.encode() not in head:
            c.note("%s: start marker %s missing: %s" % (app, start, _tail(head)))

    for delay, line, wait_marker in spec.get("pre", []):
        s.read_for(delay)
        s.write_line(line)
        if wait_marker:
            got = _read_until(s, wait_marker, spec.get("step_timeout", 45.0))
            if wait_marker.encode() not in got:
                # The line reached the shell before the modal opened (common
                # on a slow first paint). Give the list time, then resend.
                s.read_for(2.0)
                s.write_line(line)
                got += _read_until(s, wait_marker,
                                   spec.get("step_timeout", 45.0))
            if wait_marker.encode() not in got:
                c.note("%s: step marker %s missing after %r: %s"
                       % (app, wait_marker, line, _tail(got)))

    done = spec.get("done")
    if done:
        got = _read_until(s, done, spec.get("done_timeout", 240.0))
        if done.encode() not in got:
            c.note("%s: done marker %s missing: %s" % (app, done, _tail(got)))

    if spec.get("shot_delay"):
        s.read_for(spec["shot_delay"])

    bmp = None
    source = ""
    if isinstance(shot, str) and shot == "stream":
        try:
            bmp = dev.screenshot(unwind=False)
            source = "screenshot stream"
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.note("%s: streaming capture failed: %s" % (app, exc))
    elif isinstance(shot, tuple) and shot[0] == "snap":
        _kind, key, marker, remote = shot
        s.read_for(0.8)
        s.write_line(key)
        got = _read_until(s, marker, 30.0)
        if marker.encode() not in got:
            c.note("%s: snap key %r was not acknowledged: %s"
                   % (app, key, _tail(got)))
        source = "app snapshot " + remote
    elif isinstance(shot, tuple) and shot[0] == "file":
        source = "app snapshot " + shot[1]
    else:
        c.note("%s: no capture path configured" % app)

    # Quit before pulling an app-written BMP: the app owns the console while
    # it runs, so a `send`/`pull` command would be eaten as keys.
    exited = True
    if spec.get("quit") or spec.get("end") or spec.get("end_shell"):
        s.read_for(0.5)
        # Do NOT drain after the last line: the app's exit marker is emitted
        # in the next few hundred ms and `read_for` would throw it away (that
        # was why every menu app looked like it had not exited cleanly).
        quit_lines = spec.get("quit", [])
        settle = spec.get("quit_settle", 0.6)
        for index, line in enumerate(quit_lines):
            if index:
                s.read_for(settle)
            s.write_line(line)
        if spec.get("end"):
            got = _read_until(s, spec["end"], 18.0)
            exited = spec["end"].encode() in got
            if not exited:
                # A dropped quit line is common while a modal is closing.
                # One short retry is enough: `_ensure_shell` below is the
                # real "the next app can start" gate.
                for _ in range(2):
                    for line in quit_lines:
                        s.write_line(line)
                        s.read_for(0.7)
                    got += _read_until(s, spec["end"], 8.0)
                    if spec["end"].encode() in got:
                        exited = True
                        break
                if not exited:
                    c.note("%s: end marker %s missing; tail: %s"
                           % (app, spec["end"], _tail(got)))
    # The next app's launch must reach the shell, never an app that still
    # owns the input (also heals a console wedged by a large `send` frame).
    shell_ok = _ensure_shell(dev, c, app, spec.get("quit", ()))
    exited = exited and shell_ok

    if isinstance(shot, tuple) and shot[0] in ("file", "snap"):
        remote = shot[-1]
        # A short settle keeps the pull off a console that is still closing
        # the app's TUI (measured with TCMD's BMP on the Tab5).
        _run(dev, c, app + " settle", "delay 300", timeout=20)
        if bmp is None:
            bmp = _pull_bmp(dev, c, remote)
        _run(dev, c, app + " cleanup snapshot", "del /f %s" % remote,
             timeout=20)
        _ensure_shell(dev, c, app)

    ok = _publish(dev, c, app, bmp, want, source)
    if not ok:
        c.note("%s: no usable capture (%s)" % (app, source))
    elif not exited:
        c.note("%s: captured, but the app did not report a clean exit" % app)
    return ok


def _drive_background(dev, c, spec, want):
    app = spec["app"]
    out = _run(dev, c, app + " start", spec["cmd"], timeout=25)
    match = re.search(r"\[(bg\d+)\]\s+started", out)
    slot = match.group(1) if match else None
    if slot is None:
        c.note("%s: no background slot in %r" % (app, _tail(out)))
    tick_seen = spec["start"] in out
    if not tick_seen:
        got = _read_until(dev.session, spec["start"], 45.0)
        tick_seen = spec["start"].encode() in got
    if not tick_seen:
        c.note("%s: first tick %s missing: %s"
               % (app, spec["start"], _tail(out)))
    dev.session.read_for(0.6)
    bmp = None
    try:
        bmp = dev.screenshot(unwind=False)
        source = "screenshot stream"
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("%s: streaming capture failed: %s" % (app, exc))
        source = ""
    if slot:
        _run(dev, c, app + " stop", "taskkill %s" % slot, timeout=25)
    else:
        _run(dev, c, app + " stop by name", "taskkill %s" % app.upper(),
             timeout=25)
    read_marker = spec.get("quit_marker")
    if read_marker:
        _read_until(dev.session, read_marker, 15.0)
    _ensure_shell(dev, c, app)
    ok = _publish(dev, c, app, bmp, want, source)
    if not ok:
        c.note("%s: no usable capture (%s)" % (app, source))
    return ok


def _cleanup(dev, c):
    for name in TEMP_FILES:
        _run(dev, c, "cleanup %s" % name, "del /f %s" % name, timeout=20)
    for name in ("NOTES/gallery.txt", "NOTES/deepnote.txt",
                 "NOTES/suitek.txt"):
        _run(dev, c, "cleanup %s" % name, "del /f %s" % name, timeout=20)
    _run(dev, c, "cleanup draw", "draw close", timeout=20)
    _run(dev, c, "cleanup fullscreen", "draw fullscreen off", timeout=20)
    c.note("gallery artifacts cleaned; payloads left installed")


def run(dev, ctx):
    c = Checklist(NAME)

    # The apps are the surfaces under test; a TUI left open by an earlier
    # suite would make `gfx` refuse and paint over the first capture.
    try:
        dev.run("draw close", timeout=15)
        dev.run("draw fullscreen off", timeout=15)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass

    want = dev.display_size()
    if want != REFERENCE_SIZE:
        c.note("panel is %dx%d; the docs gallery is the %dx%d reference board "
               "(no PNG will be written on this panel)" % (want + REFERENCE_SIZE))

    WRITTEN.clear()
    import tempfile
    tmp = tempfile.mkdtemp(prefix="p4gallery_")
    try:
        _deploy(dev, c, tmp)

        for spec in SPECS:
            _log("driving %s" % spec["app"])
            ok = _drive(dev, c, spec, want)
            if not ok:
                _log("retrying %s" % spec["app"])
                _ensure_shell(dev, c, spec["app"])
                _drive(dev, c, spec, want)
        for spec in BG_SPECS:
            _log("driving background %s" % spec["app"])
            ok = _drive_background(dev, c, spec, want)
            if not ok:
                _log("retrying background %s" % spec["app"])
                _ensure_shell(dev, c, spec["app"])
                _drive_background(dev, c, spec, want)

        # One closing gate: every app was captured by THIS run (a stale file
        # from an older run cannot satisfy it).
        names = [spec["app"] for spec in SPECS + BG_SPECS]
        missing = [n for n in names if n not in WRITTEN]
        c.check("gallery publishes every app PNG",
                not missing and len(names) == len(set(names)),
                "missing=%r" % missing)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
        _cleanup(dev, c)

    return c
