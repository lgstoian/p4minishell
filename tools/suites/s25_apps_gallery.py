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
from p4test import theme_guard

# Pixel/rotation state from the previous suite changes these captures.
NEEDS_CLEAN_BOOT = True

NAME = "apps_gallery"
TAGS = ["apps", "gallery", "slow"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
DOCS = os.path.join(ROOT, "docs", "assets", "apps")
# A test run stages its captures here. Publishing to docs/ is a separate,
# explicitly reviewed step (tools/publish_gallery.py); a normal `git add -A`
# after a suite run must never commit a screenshot into the docs tree.
STAGING = os.path.join(ROOT, "screenshots", "gallery")
REFERENCE_SIZE = (1024, 600)

# Apps whose PNG was written by the current run (the final gate uses this so a
# stale file from an older run can never stand in for a failed capture).
WRITTEN = set()
# Apps this run tried to publish. The closing gate compares against this, not
# against every spec: a panel other than the reference one deliberately skips
# the app-written captures, and requiring them there failed the gate for apps
# the run never attempted.
ATTEMPTED = set()

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
# Apps with NO on-screen page: one-shot transcript emitters and headless
# services. They are still driven and marker-checked (functional coverage), but
# a screenshot of the shell after they exit is NOT their documentation. This is
# the exact false green that shipped 13 wrong PNGs in v1.4.3.
NON_VISUAL = {
    "agenda", "aliases", "appl", "control", "hybrid", "lib", "pkgtest",
    "uitest", "writer", "svc", "memlog", "moodsvc",
}

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
         # Dismiss the welcome dialog, then wait for the real menu marker:
         # capturing after "ok" photographed a nearly empty "Text" prompt.
         pre=[(2.0, "ok", "[M-NOTE-MENU]")], shot="stream", shot_delay=3.0,
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
         prompt="What now?",
         pre=[(1.0, "n", "---"), (0.3, "w", "---"),
              (0.3, "snap", "[M-ADV-SNAP]")],
         shot=("file", "ADVENT.BMP"), quit=["quit"], end="[M-ADV-QUIT]"),
    dict(app="snake", cmd="launch SNAKE", start="[M-SNAKE]",
         prompt="Move?",
         # Drive enough steps for a real snake before the snapshot: one key
         # left a 3-cell stub that looked like a broken render in v1.4.3.
         pre=[(1.2, "s", None), (1.0, "s", None)],
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
         pre=[(4.0, "6", "[M-FILES-NOTES]")], shot="stream", shot_delay=3.0,
         quit=["5"], end="[M-FILES-BACK]"),
    dict(app="net-msg", cmd="launch NET", start="[M-NET]",
         pre=[(4.0, "7", "[M-NET-OUTBOX]")], shot="stream", shot_delay=1.0,
         quit=["8"], quit_settle=2.0, end="[M-NET-BACK]"),
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
    # A flapping hosted Wi-Fi link emits `eh_host_feat_rpc: no response` lines
    # asynchronously; they corrupt the in-flight BMPX screenshot frames. The
    # gallery is entirely local, so disconnect before driving any app.
    try:
        dev.run("wifi disconnect", timeout=20)
    except Exception:  # noqa: BLE001 - already disconnected is fine
        pass
    _log("deploying the reference bundle")
    for name in ("APPS", "WEB", "EMOJI", "DOCS", "TEMPLATES", "DICTS",
                 "NOTES", "tmp"):
        _run(dev, c, "mkdir %s" % name, "md %s" % name, timeout=20)
    payloads = APP_PAYLOADS + COMPANION_PAYLOADS
    pushed = dev.deploy_cached(
        [(payload_path(subdir, name), remote_target(name, target))
         for subdir, name, target in payloads],
        lambda local, remote: _push(dev, c, local, remote))
    _log("app bundle %s" % ("pushed" if pushed else "already current"))
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


def _publish(dev, c, app, bmp, want, source, identity_ok=True):
    """Write the PNG and run the one-per-app gallery gate."""
    ATTEMPTED.add(app)
    if not identity_ok:
        # The app never proved it was on its own screen. Publishing the capture
        # is what shipped 14 shell-transcript PNGs as app documentation.
        c.check("%s: on-screen identity verified before capture" % app,
                False, "the app's start/done/snap markers were not all seen")
        return False
    check_name = ("%s: PNG is a valid %dx%d image and not blank"
                  % (app, want[0], want[1]))
    if bmp is None:
        c.check(check_name, False, "no capture from %s" % (source or "the app"))
        return False

    path = os.path.join(STAGING, app + ".png")
    try:
        img = _image_from_bmp(bmp)
    except Exception as exc:  # noqa: BLE001
        c.check("%s: PNG is a valid %dx%d image and not blank"
                % (app, want[0], want[1]), False,
                "%s BMP did not decode: %s" % (source, exc))
        return False

    size_ok = (img.width, img.height) == want
    # Only the reference panel publishes docs PNGs (see the note above); on any
    # other panel the PNG is intentionally not written, so its absence is not a
    # failure - the capture itself (size, ink, colours) still has to hold.
    png_ok = want != REFERENCE_SIZE
    if size_ok and want == REFERENCE_SIZE:
        os.makedirs(STAGING, exist_ok=True)
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
    ok = size_ok and png_ok and not_blank and identity_ok
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
    # The LVGL default-theme primary is never part of this UI: a sized blob of
    # it is a widget we forgot to style. The input-row buttons used to leak
    # exactly this, which read as a blue flash whenever they reappeared.
    hits = theme_guard.scan_image(img)
    c.check("%s: no unstyled default-theme widget" % app, not hits,
            theme_guard.describe(hits))
    if ok:
        WRITTEN.add(app)
        c.note("%s: %dx%d, %.1f%% ink, %d colours" %
               (app, img.width, img.height, ink * 100.0, colours))
    return ok


def _pull_bmp(dev, c, remote):
    """Pull an app-written BMP and parse it through the shared pixel model.

    The app writes the file from its own key handler and a key can be delivered
    a moment after the snapshot is asked for; one retry after a short settle
    keeps a correct screen from failing on the write/pull race (SNAKE's capture
    came back missing once in a full sweep).
    """
    data = None
    for attempt in range(2):
        try:
            data = dev.pull_file(remote)
            break
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            if attempt == 1:
                c.note("pull %s failed: %s" % (remote, exc))
                return None
            time.sleep(1.0)
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
    """Drive one app, prove it reached its own screen, then capture it.

    Two rules this harness broke in v1.4.3:

      * a stream screenshot must be taken WHILE the app is on screen - waiting
        for the app's done/exit marker first photographs the shell prompt and
        publishes it as the app's documentation;
      * a missing identity marker is a FAILED CHECK, never a note.

    Non-visual apps (one-shot transcript emitters and headless services) are
    still driven and marker-checked here, but they publish no PNG: they have no
    screen to photograph. Functional coverage is unchanged; only the misleading
    screenshot is gone.
    """
    app = spec["app"]
    s = dev.session
    identity_ok = True
    shot = spec.get("shot")
    visual = app not in NON_VISUAL

    # Delete the app's own snapshot first so a stale file can never be taken
    # for this run's capture (a failed delete is reported by the pull gate).
    if isinstance(shot, tuple) and shot[0] in ("file", "snap"):
        _run(dev, c, app + " clear snapshot", "del /f %s" % shot[-1],
             timeout=20)

    start_marker = spec.get("start")
    if start_marker is None:
        _run(dev, c, app + " surface", spec["cmd"], timeout=60)
    else:
        s.reset_input()
        s.write_line(spec["cmd"])
        head = _read_until(s, start_marker, spec.get("timeout", 60.0))
        seen = start_marker.encode() in head
        c.check("%s reached its start marker %s" % (app, start_marker),
                seen, _tail(head))
        identity_ok = identity_ok and seen

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
            seen = wait_marker.encode() in got
            c.check("%s reached step marker %s" % (app, wait_marker),
                    seen, _tail(got))
            identity_ok = identity_ok and seen

    bmp = None
    source = ""
    # --- Capture while the app owns the screen (stream / snap) -------------
    if visual and shot == "stream":
        if spec.get("shot_delay"):
            s.read_for(spec["shot_delay"])
        for attempt in range(2):
            try:
                bmp = dev.screenshot(unwind=False)
                source = "screenshot stream"
                break
            except PanicError:
                raise
            except Exception as exc:  # noqa: BLE001
                # Both attempts are recorded. A first-attempt failure is a
                # failed check even if a retry later succeeds: hiding it would
                # be exactly the retry-masked false green this harness had.
                c.check("%s: streaming capture attempt %d"
                        % (app, attempt + 1), False, str(exc))
                if attempt == 0:
                    s.read_for(1.0)
                    s.reset_input()
    elif visual and isinstance(shot, tuple) and shot[0] == "snap":
        _kind, key, marker, remote = shot
        for attempt in range(2):
            s.read_for(0.5)
            s.read_until(spec.get("prompt", "key?").encode(), 20.0)
            s.read_for(0.3)
            s.write_line(key)
            got = _read_until(s, marker, 20.0)
            seen = marker.encode() in got
            # Record every attempt: a first failure stays red even if the
            # second snap lands (no retry-masked green).
            c.check("%s snap key %r attempt %d" % (app, key, attempt + 1),
                    seen, _tail(got))
            if seen:
                break
        identity_ok = identity_ok and seen
        source = "app snapshot " + remote

    done = spec.get("done")
    if done:
        got = _read_until(s, done, spec.get("done_timeout", 240.0))
        seen = done.encode() in got
        c.check("%s reached its done marker %s" % (app, done),
                seen, _tail(got))
        identity_ok = identity_ok and seen

    # --- Quit before pulling an app-written BMP ----------------------------
    exited = True
    if spec.get("quit") or spec.get("end") or spec.get("end_shell"):
        s.read_for(0.5)
        s.read_until(spec.get("prompt", "key?").encode(), 20.0)
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
                for _ in range(2):
                    for line in quit_lines:
                        s.write_line(line)
                        s.read_for(0.7)
                    got += _read_until(s, spec["end"], 8.0)
                    if spec["end"].encode() in got:
                        exited = True
                        break
                if not exited:
                    c.check("%s reached its exit marker %s"
                            % (app, spec["end"]), False, _tail(got))
                    identity_ok = False
    shell_ok = _ensure_shell(dev, c, app, spec.get("quit", ()))
    exited = exited and shell_ok

    if visual and isinstance(shot, tuple) and shot[0] in ("file", "snap"):
        remote = shot[-1]
        _run(dev, c, app + " settle", "delay 300", timeout=20)
        if bmp is None:
            bmp = _pull_bmp(dev, c, remote)
        _run(dev, c, app + " cleanup snapshot", "del /f %s" % remote,
             timeout=20)
        _ensure_shell(dev, c, app)

    if not visual:
        # Functional verification for transcript emitters / headless services:
        # no screenshot is published, but a missing marker still fails.
        c.check("%s functional markers verified" % app, identity_ok,
                _tail(source or "no visual surface by design"))
        return True

    if app in NON_VISUAL:
        c.check("%s service tick verified" % app, identity_ok,
                _tail(out))
        return True
    ok = _publish(dev, c, app, bmp, want, source, identity_ok)
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
    c.check("%s produced its first tick %s" % (app, spec["start"]),
            tick_seen, _tail(out))
    identity_ok = tick_seen
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
    if app in NON_VISUAL:
        c.check("%s service tick verified" % app, identity_ok,
                _tail(out))
        return True
    ok = _publish(dev, c, app, bmp, want, source, identity_ok)
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
    ATTEMPTED.clear()
    if os.path.isdir(STAGING):
        for stale in os.listdir(STAGING):
            if stale.lower().endswith(".png"):
                os.remove(os.path.join(STAGING, stale))
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
        names = [spec["app"] for spec in SPECS + BG_SPECS
                 if spec["app"] in ATTEMPTED]
        missing = [n for n in names if n not in WRITTEN]
        if want == REFERENCE_SIZE:
            c.check("gallery stages every app PNG",
                    not missing and len(names) == len(set(names)),
                    "staged=%r missing=%r" % (sorted(WRITTEN), missing))
        else:
            c.note("no PNG is staged on this panel; the docs gallery is the "
                   "%dx%d reference board" % REFERENCE_SIZE)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
        _cleanup(dev, c)

    return c
