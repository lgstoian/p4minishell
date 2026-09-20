# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Editor suite: drive the modal ``edit`` surface over the serial console.

The editor blocks the command worker, so its serial verbs are written as raw
lines (``dev.session.write_line``/``dev.send``) rather than marker-synced
``dev.run`` - an ``echo`` marker would otherwise be typed into the document.
Openness is confirmed through ``ui state`` (handled by the console reader while
a modal owns the screen), and the streaming screenshot works during the modal
too. After each session the shell is allowed to come back and the saved bytes
are checked with ``type`` / ``findstr``.

Covers: push a generated file, ``\\g`` goto, ``\\f`` find, ``\\u`` undo,
``\\r`` redo, ``\\s`` save, ``\\o`` save-as, ``\\q`` quit, the edit-page verbs,
and a few-thousand-line PSRAM document.
"""
import os
import re
import time

from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "editor"
TAGS = ["editor", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

FILE_A = "_S09A.TXT"
FILE_C = "_S09C.TXT"
FILE_D = "_S09D.BAT"
FILE_BIG = "_S09BIG.TXT"
BIG_LINES = 2000
BIG_MARK = "ZZBIGENDMARK"


def _field(text, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), text)
    return m.group(1) if m else None


def _ui_state(dev, settle=1.0):
    try:
        # Reset first so a poll only sees its own reply, never stale output.
        dev.session.reset_input()
        dev.session.write_line("ui state")
        return dev.read_for(settle)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        return ""


def _wait_editor(dev, want, timeout=15.0):
    end = time.time() + timeout
    st = None
    while time.time() < end:
        st = _field(_ui_state(dev), "editor")
        if st == want:
            return st
        time.sleep(0.3)
    return st


def _wait_field(dev, key, want, timeout=15.0):
    end = time.time() + timeout
    val = None
    while time.time() < end:
        val = _field(_ui_state(dev), key)
        if val == want:
            return val
        time.sleep(0.3)
    return val


def run(dev, ctx):
    c = Checklist(NAME)
    disp_w, disp_h = dev.display_size()
    created = [FILE_A, FILE_C, FILE_D, FILE_BIG]

    def shot(name):
        try:
            return dev.screenshot(out_dir=OUT_DIR, name=name)
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.note("screenshot %s failed: %s" % (name, exc))
            return None

    try:
        # =================================================================
        # 1. Small file: goto / type / undo / redo / find / save / save-as
        # =================================================================
        c.note("editor: small file session")
        a_lines = ["EDITOR SUITE FILE A", "SECOND LINE", "THIRD LINE", "END OF FILE A"]
        dev.push_file(FILE_A, ("\r\n".join(a_lines) + "\r\n").encode())

        dev.send("edit %s" % FILE_A, 6.0)
        st = _wait_editor(dev, "open", timeout=15)
        c.check("editor opened", st == "open", "state=%r" % st)
        small_state = _ui_state(dev)
        c.check("editor nav capability on", _field(small_state, "nav") == "on",
                "nav=%r" % _field(small_state, "nav"))
        lines = _field(small_state, "lines")
        c.check("editor loaded lines", lines is not None and int(lines) >= 4,
                "lines=%r" % lines)

        bmp = shot("s09_editor_open")
        if bmp is not None:
            c.equals("editor screenshot width", bmp.width, disp_w)
            c.equals("editor screenshot height", bmp.height, disp_h)
            c.check("editor surface visible",
                    sum(bmp.region_mean(0, 40, disp_w, disp_h - 40 * 2)) > 10,
                    "mean=%r" % (bmp.region_mean(0, 40, disp_w, disp_h - 40 * 2),))
        else:
            c.check("editor screenshot captured", False, "screenshot failed")

        # Go to line 2 and type a marker, then exercise undo/redo.
        dev.send("\\g", 1.5)
        dev.send("2", 2.0)
        dev.send("ZZEDMARKER", 1.5)
        c.check("editor reports modified",
                _wait_field(dev, "modified", "1", timeout=6) == "1",
                "state=%r" % _ui_state(dev)[-160:])
        dev.send("\\u", 1.5)
        dev.send("\\r", 1.5)
        # Find is prompt-driven: `\f` then the needle on the next line.
        dev.send("\\f", 1.5)
        dev.send("SECOND", 2.0)
        dev.send("\\s", 6.0)
        c.check("editor clean after save",
                _wait_field(dev, "modified", "0", timeout=10) == "0",
                "state=%r" % _ui_state(dev)[-160:])

        dev.send("\\q", 5.0)
        dev.send("y", 2.5)
        c.check("editor closed", _wait_editor(dev, "closed", timeout=12) == "closed",
                "state=%r" % _ui_state(dev)[-160:])

        out_a = dev.run("type %s" % FILE_A, timeout=20)
        c.expect("file A keeps header", "EDITOR SUITE FILE A", out_a)
        c.expect("file A keeps marker", "ZZEDMARKER", out_a)

        # Save As from an *unnamed* buffer: the prompt is then empty, so the
        # typed path is not appended to a pre-filled current path.
        dev.send("edit", 6.0)
        c.check("unnamed editor opened",
                _wait_editor(dev, "open", timeout=15) == "open",
                "state=%r" % _ui_state(dev)[-160:])
        dev.send("UNNAMED LINE ONE", 1.5)
        dev.send("\\o", 2.0)
        dev.send(FILE_C, 3.0)
        dev.send("ZZCMARKER", 1.5)
        dev.send("\\s", 6.0)
        dev.send("\\q", 5.0)
        dev.send("y", 2.5)
        c.check("editor closed after save-as",
                _wait_editor(dev, "closed", timeout=12) == "closed",
                "state=%r" % _ui_state(dev)[-160:])

        out_c = dev.run("type %s" % FILE_C, timeout=20)
        c.expect("save-as created file C", "ZZCMARKER", out_c)
        c.expect("save-as carried document", "UNNAMED LINE ONE", out_c)

        # =================================================================
        # 2. Edit-page / nav-page serial verbs smoke
        # =================================================================
        c.note("editor: nav/edit verb smoke")
        dev.send("edit %s" % FILE_D, 6.0)
        c.check("editor reopened", _wait_editor(dev, "open", timeout=15) == "open",
                "state=%r" % _ui_state(dev)[-160:])
        dev.send("ECHO HI", 1.5)
        for verb in ("\\co", "\\w", "\\b", "\\l", "\\p", "\\a"):
            dev.send(verb, 1.2)
        dev.send("\\q", 4.0)
        dev.send("y", 2.5)
        c.check("editor closed after smoke",
                _wait_editor(dev, "closed", timeout=12) == "closed",
                "state=%r" % _ui_state(dev)[-160:])

        # =================================================================
        # 3. Large file: PSRAM document + render window
        # =================================================================
        c.note("editor: %d-line document" % BIG_LINES)
        big = "".join("L%05d: padding text for the large-file editor suite\n" % i
                      for i in range(BIG_LINES))
        dev.push_file(FILE_BIG, big.encode())

        dev.send("edit %s" % FILE_BIG, 12.0)
        st = _wait_editor(dev, "open", timeout=25)
        c.check("big editor opened", st == "open", "state=%r" % st)
        if st == "open":
            lines = _field(_ui_state(dev), "lines")
            c.check("big document line count", lines is not None and int(lines) >= BIG_LINES,
                    "lines=%r" % lines)
            # Jump to the end and append the marker, then save (and wait for
            # the clean flag) before quitting.
            dev.send("\\g", 2.0)
            dev.send(str(BIG_LINES), 5.0)
            dev.send(BIG_MARK, 2.0)
            dev.send("\\s", 8.0)
            _wait_field(dev, "modified", "0", timeout=25)
            dev.send("\\q", 8.0)
            dev.send("y", 3.0)
            c.check("big editor closed",
                    _wait_editor(dev, "closed", timeout=20) == "closed",
                    "state=%r" % _ui_state(dev)[-160:])
            out = dev.run('findstr /C:"%s" %s' % (BIG_MARK, FILE_BIG), timeout=30)
            c.expect("big marker persisted", BIG_MARK, out)
        else:
            c.check("big editor closed", False, "editor did not open")

        # The shell must be fully restored and functional.
        c.expect("shell restored", "EDITOR_SUITE_DONE",
                 dev.run("echo EDITOR_SUITE_DONE"))

    finally:
        # Never leave the editor modal owning the worker: a later `del` would
        # otherwise be typed into the document instead of run.
        try:
            for _ in range(4):
                if _field(_ui_state(dev), "editor") == "closed":
                    break
                dev.send("\\q", 0.9)
                dev.send("y", 0.7)
                time.sleep(0.3)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
        for path in created:
            try:
                dev.run("del %s" % path, timeout=10)
            except PanicError:
                raise
            except Exception:  # noqa: BLE001
                pass

    return c
