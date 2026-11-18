# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Render suite: opt-in transparency (``alpha=``/``scrim=``) + retro emoji.

v1.4.3 added two presentation features that only exist on the panel:

* per-app transparency. An ``APPINFO`` may carry ``alpha=`` (the whole TUI
  surface and every modal panel) and ``scrim=`` (a black backdrop behind
  modal panels); ``launch`` applies them for the app's lifetime and restores
  the opaque defaults afterwards (``components/command/command.c`` ->
  ``tui_set_background_alpha``/``modal_surf_set_alpha``);
* the retro emoji fallback. ``tui_print_at`` rewrites a codepoint the built-in
  pack cannot draw through the MIT DOS symbol table (``components/font/
  emoji_core.c``), so U+1F600 is stored/drawn as U+263A.

Transparency is measured on TCMD (``apps/tcmd``): the same app is launched
three times with two temporary ``APPINFO`` manifests - opaque, opaque again,
then ``alpha=40%``/``scrim=40%`` - and each run snapshots its own screen while
the TUI is up, using TCMD's documented ``p`` snap key (``screenshot TCMD.BMP``).
A host streaming ``screenshot`` cannot be used there: TCMD sits in a ``choice``
key wait that owns the console input, and the shell forwards every serial line
to that key wait before it ever reaches a command (``components/shell/
shell.c``), so the capture would be eaten as keystrokes. The app-written BMP is
the deterministic path (the same one s21 uses for ``screenshot <file>``).
The second opaque launch is the control: it must be pixel-identical to the
first, which proves the change between the opaque and translucent shots is the
APPINFO, not run-to-run noise.

The alpha blend on this panel is deliberately subtle (the surface background
is composited over a near-black screen), so the gate is the *fraction* of
pixels that moved: most of the background moves by a few levels while the
glyph ink stays identical. That is the documented behaviour of a background
opacity, and it is asserted in both directions (blend present, nothing
structural changed, text not washed out).

The emoji leg draws U+1F600 and then the literal U+263A at the same cell and
snapshots both. ``tui dump`` cannot show a ``draw text`` cell: it reads the
*browser* page model (``tui_page_get_cell``, initialised by ``browser``), so on
a TUI-only page every dumped row is empty - verified on hardware (the fixture
runs ``tui dump`` and asserts the verb ran). Asserting the two screenshots are
pixel-identical is stronger than "the cell is not blank": it pins the actual
retro glyph. A third snapshot with the cell cleared proves the glyph cell has
ink at all.

Everything the suite pushes is temporary: fixtures are deleted, the previous
``sd:/APPS/TCMD.APPINFO`` is restored byte-for-byte (or removed when there was
none), and TCMD.BAT/APPLIB.BAT are left installed exactly as s14 leaves them.
The suite reboots before it starts: the transcript surface keeps stale LVGL
background state across a session (after enough app/modal runs the surface
background reads as black, which hides every alpha blend until a power-cycle -
measured on COM3), so the pixel comparison must start from a clean boot.
"""
from __future__ import annotations

import os

from p4test.asserts import Checklist
from p4test.session import PanicError
from p4test.screenshot import diff_ratio, parse_bmp

NAME = "render_alpha"
TAGS = ["ui", "apps", "gfx"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
OUT_DIR = os.path.join("screenshots", "regression")

APPINFO_REMOTE = "APPS/TCMD.APPINFO"

# Opaque manifest: every transparency key absent = the documented default.
OPAQUE_INFO = (
    "title=Commander\r\n"
    "description=Dual-pane file commander\r\n"
    "version=1.0\r\n"
)
# Strong (not maximal) translucency so the blend is unmistakable; 0-100% is the
# documented form and the harness reads both values from the manifest.
ALPHA_INFO = OPAQUE_INFO + "alpha=40%\r\nscrim=40%\r\n"

# A dialog fixture: launches with alpha/scrim active, draws a page, then opens
# the real `dialog` modal with a bounded timeout so an unattended run cannot
# hang. `/t:3` is the documented auto-cancel form.
DIALOG_INFO = (
    "title=Alpha probe\r\n"
    "description=Transparency probe\r\n"
    "version=1.0\r\n"
    "alpha=60%\r\n"
    "scrim=40%\r\n"
)
DIALOG_BAT = (
    "@echo off\r\n"
    "draw hold on\r\n"
    "draw fullscreen on\r\n"
    "draw box 1 1 80 25 single\r\n"
    "draw text 3 3 \"transparency probe\"\r\n"
    "draw refresh\r\n"
    "echo [M-ALPHA-READY]\r\n"
    "dialog /t:3 \"Alpha probe\" \"Modal panel and scrim\"\r\n"
    "echo [M-ALPHA-DIALOG-DONE]\r\n"
    "draw fullscreen off\r\n"
    "draw close\r\n"
    "echo [M-ALPHA-DONE]\r\n"
)

# Emoji fixture: three snapshots of the same page, differing only in cell (3,3)
# - the emoji, the retro fallback glyph, and blank (to prove the cell has ink).
EMOJI_BAT = (
    "@echo off\r\n"
    "draw hold on\r\n"
    "draw fullscreen on\r\n"
    "draw box 1 1 80 25 single\r\n"
    "draw text 3 3 \"\U0001F600\"\r\n"
    "draw refresh\r\n"
    "tui dump 1 5\r\n"
    "screenshot EMOJI.BMP\r\n"
    "draw text 3 3 \"\u263A\"\r\n"
    "draw refresh\r\n"
    "screenshot EMOJIRE.BMP\r\n"
    "draw clear\r\n"
    "draw box 1 1 80 25 single\r\n"
    "draw refresh\r\n"
    "screenshot EMOJIBLANK.BMP\r\n"
    "draw fullscreen off\r\n"
    "draw close\r\n"
    "echo [M-EMOJI-DONE]\r\n"
)


def _tail(data) -> str:
    if isinstance(data, bytes):
        data = data.decode("utf-8", "replace")
    return data[-300:].replace("\r", "")


def _push(dev, c, local, remote):
    try:
        dev.push_local(local, remote)
        return True
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("push %s" % remote, False, str(exc))
        return False


def _pull_bmp(dev, c, remote, label, keep=None):
    """Pull a device-written BMP, validate the magic, and return a pixel model."""
    try:
        data = dev.pull_file(remote)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("%s: BMP pulled" % label, False, str(exc))
        return None
    finally:
        try:
            dev.run("del /f %s" % remote, timeout=20)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    ok = data[:2] == b"BM" and len(data) > 1000
    c.check("%s: BMP is valid" % label, ok,
            "%d bytes, magic=%r" % (len(data), data[:2]))
    if not ok:
        return None
    if keep is not None:
        try:
            os.makedirs(OUT_DIR, exist_ok=True)
            with open(os.path.join(OUT_DIR, keep), "wb") as fh:
                fh.write(data)
        except OSError as exc:  # noqa: BLE001 - artifact only, not a gate
            c.note("could not save %s: %s" % (keep, exc))
    try:
        return parse_bmp(data)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("%s: BMP parses" % label, False, str(exc))
        return None


def _snap_tcmd(dev, c, label):
    """Launch TCMD, snapshot its TUI with the documented ``p`` snap key, quit.

    TCMD owns the console input in its ``choice`` key loop, so every send is a
    key and the batch is driven exactly like s14 does: sync on ``key?``, then
    one letter per line. The quit is the documented ``q`` (then Enter); a
    bounded retry keeps a dropped key from leaving the app open for the next
    run.
    """
    session = dev.session
    session.reset_input()
    session.write_line("launch TCMD")
    head = session.read_until(b"[M-TCMD-R]", 60)
    c.check("%s: TCMD drew its TUI" % label, b"[M-TCMD-R]" in head,
            _tail(head))
    session.read_for(1.0)
    session.write_line("p")          # documented snap key -> screenshot TCMD.BMP
    tail = session.read_until(b"[M-TCMD-R]", 25)
    c.check("%s: snap accepted and the pane refreshed" % label,
            b"[M-TCMD-R]" in tail, _tail(tail))
    session.read_for(0.8)
    session.write_line("q")          # documented quit key (letter + Enter)
    tail = session.read_until(b"[M-TCMD-DONE]", 15)
    exited = b"[M-TCMD-DONE]" in tail
    if not exited:
        for _ in range(3):
            session.write_line("q")
            tail = session.read_until(b"[M-TCMD-DONE]", 8)
            if b"[M-TCMD-DONE]" in tail:
                exited = True
                break
    c.check("%s: TCMD quit cleanly" % label, exited, _tail(tail))
    if not exited:
        # Keep the next launch from being eaten by a still-open key wait.
        for _ in range(3):
            session.write_line("q")
            session.read_for(0.5)
    dev.run("delay 300", timeout=25)
    return _pull_bmp(dev, c, "TCMD.BMP", "%s: TCMD screen" % label)


def _bright_samples(bmp, threshold=500):
    """Count near-white samples; opaque glyph ink must survive the blend."""
    n = 0
    for y in range(0, bmp.height, 4):
        for x in range(0, bmp.width, 4):
            if sum(bmp.px(x, y)) > threshold:
                n += 1
    return n


def _diff_samples(a, b, tol=0):
    """How many sampled pixels moved by more than ``tol`` levels."""
    n = 0
    for y in range(0, a.height, 4):
        for x in range(0, a.width, 4):
            pa = a.px(x, y)
            pb = b.px(x, y)
            if (abs(pa[0] - pb[0]) > tol or abs(pa[1] - pb[1]) > tol
                    or abs(pa[2] - pb[2]) > tol):
                n += 1
    return n


def _dialog_leg(dev, c):
    """`dialog /t:3` must still render while ``scrim`` is set, and not panic.

    The streaming capture is the modal-readiness proof: the bare ``screenshot``
    token is claimed by the console-reader *only* while a native modal blocks
    the worker (``command_modal_console_command``). With the fixture's batch
    owning the worker, a successful capture therefore means the dialog was up
    at that instant; without it the line would merely queue and the capture
    would time out. `/t:3` then cancels the dialog, so the suite cannot hang.
    """
    dev.push_file("ALPHADLG.BAT", DIALOG_BAT.encode())
    dev.push_file("APPS/ALPHADLG.APPINFO", DIALOG_INFO.encode())
    session = dev.session
    session.reset_input()
    session.write_line("launch ALPHADLG")
    head = session.read_until(b"[M-ALPHA-READY]", 40)
    c.check("dialog: fixture drew the page", b"[M-ALPHA-READY]" in head,
            _tail(head))
    session.read_for(0.5)   # let the dialog come up before the capture
    shot = None
    try:
        shot = dev.screenshot(unwind=False)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("dialog: captured while the modal was up", False, str(exc))
    if shot is not None:
        c.check("dialog: captured while the modal was up",
                shot.width > 100 and shot.height > 100,
                "%dx%d" % (shot.width, shot.height))
    tail = session.read_until(b"[M-ALPHA-DONE]", 30)
    text = tail.decode("utf-8", "replace")
    c.check("dialog: /t:3 auto-cancelled and the app completed",
            "[M-ALPHA-DONE]" in text, _tail(tail))
    c.check("dialog: no panic with scrim set",
            "Guru Meditation" not in text and "assert failed" not in text,
            _tail(tail))
    dev.run("delay 300", timeout=25)


def _emoji_leg(dev, c):
    dev.push_file("FIXEMOJI.BAT", EMOJI_BAT.encode())
    out = dev.run("launch FIXEMOJI", timeout=90)
    c.check("emoji: fixture completed", "[M-EMOJI-DONE]" in out, _tail(out))
    # `tui dump` runs (header) but cannot see a draw cell: it reads the browser
    # page model, and this page was never rendered by `browser` (rows are empty
    # by design). Assert the verb ran so the limitation is visible in the log.
    c.check("emoji: tui dump ran (browser page model, no draw cells)",
            "tui.dump cols=" in out, _tail(out))
    c.check("emoji: no panic while drawing U+1F600",
            "Guru Meditation" not in out and "assert failed" not in out,
            _tail(out))

    emoji = _pull_bmp(dev, c, "EMOJI.BMP", "emoji: U+1F600 cell",
                      keep="render_alpha_emoji.bmp")
    retro = _pull_bmp(dev, c, "EMOJIRE.BMP", "emoji: U+263A reference")
    blank = _pull_bmp(dev, c, "EMOJIBLANK.BMP", "emoji: blank cell")
    if not (emoji and retro and blank):
        return

    same = diff_ratio(emoji, retro, tol=0)
    c.check("emoji: the emoji cell renders as the retro U+263A glyph",
            same <= 0.005,
            "%.4f of sampled pixels differ (a mid-refresh row can move a "
            "handful); the glyph cell must be pixel-identical" % same)
    ink = _diff_samples(emoji, blank, tol=0)
    c.check("emoji: the glyph cell is not blank", ink >= 3,
            "%d sampled pixels differ from a cleared page" % ink)
    c.check("emoji: the retro reference cell is not blank",
            _diff_samples(retro, blank, tol=0) >= 3, "")
    text_ink = _bright_samples(emoji)
    c.check("emoji: the page rendered its glyphs", text_ink > 100,
            "%d bright samples" % text_ink)

    # The LVGL chrome (transcript), not the page: a symbol the cell font ships
    # (U+2605) must not render as the same placeholder as a codepoint nothing
    # has (U+E000, private use). Before the fallback was fixed both fell
    # through to FontAwesome and drew the same box, so these two captures were
    # pixel-identical.
    dev.run("cls", timeout=20)
    dev.run("echo CHROME \u2605 \u2605 \u2605 \u2605 \u2605", timeout=20)
    star = dev.screenshot()
    dev.run("cls", timeout=20)
    dev.run("echo CHROME \ue000 \ue000 \ue000 \ue000 \ue000", timeout=20)
    box = dev.screenshot()
    moved = diff_ratio(star, box, tol=0)
    c.check("emoji: a symbol renders in the transcript (not the placeholder)",
            moved >= 0.0005, "%.5f of sampled pixels differ" % moved)


def _compare(c, control, opaque, translucent):
    """Assert the transparency pair, using the control run as the noise floor."""
    noise = diff_ratio(opaque, control, tol=1)
    c.check("transparency: two opaque launches are identical", noise <= 0.005,
            "%.3f of sampled pixels differ between the two opaque runs"
            % noise)
    moved = diff_ratio(opaque, translucent, tol=1)
    c.check("transparency: the TUI surface background blended",
            moved >= 0.30,
            "%.3f of sampled pixels moved (alpha=40%% composited over the "
            "near-black page; the per-channel delta is small by design)" % moved)
    structural = diff_ratio(opaque, translucent, tol=16)
    c.check("transparency: only a blend changed (same screen content)",
            structural <= 0.01,
            "%.3f of sampled pixels changed by more than 16 levels"
            % structural)
    ink_a = _bright_samples(opaque)
    ink_b = _bright_samples(translucent)
    delta = abs(ink_a - ink_b) / float(max(1, ink_a))
    c.check("transparency: text ink stays opaque", delta <= 0.02,
            "%d vs %d bright samples (%.1f%%)" % (ink_a, ink_b, delta * 100.0))


def run(dev, ctx):
    c = Checklist(NAME)

    # Start from a clean boot: the TUI/transcript surface accumulates stale
    # LVGL background state across a session (after enough app/modal runs the
    # surface background reads as black, which hides every alpha blend until a
    # power-cycle - measured on COM3). The runner's own convention for
    # pixel-sensitive areas is a clean boot (browser/editor/apps), so this is
    # the same rule: the comparison must not depend on what ran before it.
    try:
        dev.reboot(settle_ms=4000 if ctx.get("quick") else 8000)
        c.note("clean boot before the pixel comparison")
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("clean boot before the pixel comparison", False, str(exc))
        return c

    # Defensive: a previous app/run may still hold the display.
    try:
        dev.run("draw close", timeout=20)
        dev.run("draw fullscreen off", timeout=20)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass

    saved_info = None
    try:
        saved_info = dev.pull_file(APPINFO_REMOTE)
        c.note("saved the installed TCMD.APPINFO (%d bytes)" % len(saved_info))
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        c.note("no installed TCMD.APPINFO to save (fresh board)")

    pushed = True
    try:
        dev.run("md APPS", timeout=20)
        dev.run("md tmp", timeout=20)
        pushed &= _push(dev, c, os.path.join(APPS, "tcmd", "TCMD.BAT"),
                        "TCMD.BAT")
        pushed &= _push(dev, c, os.path.join(APPS, "APPLIB.BAT"), "APPLIB.BAT")
        if not pushed:
            return c

        listed = dev.run("launch /list", timeout=30)
        c.expect("transparency: TCMD is installed", "TCMD", listed)

        # 1) opaque baseline, 2) opaque control, 3) translucent.
        dev.push_file(APPINFO_REMOTE, OPAQUE_INFO.encode())
        opaque = _snap_tcmd(dev, c, "opaque #1")
        control = _snap_tcmd(dev, c, "opaque #2")
        dev.push_file(APPINFO_REMOTE, ALPHA_INFO.encode())
        translucent = _snap_tcmd(dev, c, "alpha=40%")
        if opaque and control and translucent:
            for bmp, name in ((opaque, "render_alpha_opaque.bmp"),
                              (translucent, "render_alpha_translucent.bmp")):
                try:
                    os.makedirs(OUT_DIR, exist_ok=True)
                    with open(os.path.join(OUT_DIR, name), "wb") as fh:
                        fh.write(bmp.data)
                except OSError as exc:  # noqa: BLE001 - artifact only
                    c.note("could not save %s: %s" % (name, exc))
            _compare(c, control, opaque, translucent)

        _dialog_leg(dev, c)
        _emoji_leg(dev, c)
    finally:
        # Restore the board: the temporary manifest and every fixture go away.
        try:
            if saved_info is not None:
                dev.push_file(APPINFO_REMOTE, saved_info)
                c.note("restored the previous TCMD.APPINFO")
            else:
                dev.run("del /f %s" % APPINFO_REMOTE, timeout=20)
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.check("transparency: temporary TCMD.APPINFO cleaned up", False,
                    str(exc))
        for remote in ("FIXEMOJI.BAT", "ALPHADLG.BAT", "APPS/ALPHADLG.APPINFO",
                       "TCMD.BMP", "EMOJI.BMP", "EMOJIRE.BMP", "EMOJIBLANK.BMP"):
            try:
                dev.run("del /f %s" % remote, timeout=20)
            except PanicError:
                raise
            except Exception:  # noqa: BLE001
                pass
        try:
            dev.run("draw close", timeout=20)
            dev.run("draw fullscreen off", timeout=20)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass

    return c
