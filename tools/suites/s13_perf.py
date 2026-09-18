# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Performance + graphical frame-smoothness suite.

Deploys the three animation/diagnostic demos (BOUNCE, SNAKE, DIAG), then:

* runs BOUNCE and parses the firmware-measured ``gfx stats:`` line
  (frames / min / avg / max / jitter / dropped, via
  :func:`p4test.perf.parse_perf_report`);
* times the ``[M-BOUNCE-FRAME]`` stream on the host with
  :func:`p4test.perf.measure_markers` (a serial-bound cross-check);
* drives a short SNAKE session and parses ``tui stats:``;
* optionally drives DIAG's menu into its benchmark section;
* asserts a host-side screenshot differs between two animation frames.

The gfx benchmark belongs to BOUNCE; DIAG is best-effort because it is
menu-driven (a failure there is a note, not a suite failure).
"""
from __future__ import annotations

import os
import time

from p4test import screenshot as p4shot
from p4test.asserts import Checklist
from p4test.perf import measure_markers, parse_perf_report
from p4test.session import PanicError

NAME = "perf"
TAGS = ["perf", "slow"]
OUT_DIR = os.path.join("screenshots", "regression")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")
FILES = [
    (os.path.join(APPS, "gfxdemo", "BOUNCE.BAT"), "BOUNCE.BAT"),
    (os.path.join(APPS, "gfxdemo", "BOUNCE.APPINFO"), "APPS/BOUNCE.APPINFO"),
    (os.path.join(APPS, "diag", "DIAG.BAT"), "DIAG.BAT"),
    (os.path.join(APPS, "diag", "DIAG.APPINFO"), "APPS/DIAG.APPINFO"),
    (os.path.join(APPS, "snake", "SNAKE.BAT"), "SNAKE.BAT"),
    (os.path.join(APPS, "snake", "SNAKE.APPINFO"), "APPS/SNAKE.APPINFO"),
]


def _run(dev, c, label, cmd, timeout=30.0, settle=0.5):
    try:
        return dev.run(cmd, timeout=timeout, settle=settle)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check(label, False, "%r did not return: %s" % (cmd, exc))
        return ""


def _stat_line(text, prefix):
    for line in text.splitlines():
        if prefix.lower() in line.lower():
            return line.strip()
    return ""


def _deploy(dev, c, files):
    _run(dev, c, "mkdir APPS", "md APPS", timeout=15)
    for local, remote in files:
        try:
            dev.push_local(local, remote)
            c.check("push %s" % remote, True)
        except PanicError:
            raise
        except Exception as exc:  # noqa: BLE001
            c.check("push %s" % remote, False, str(exc))


def _diag_benchmark(dev, c):
    """Best-effort drive of DIAG -> Benchmark; never fails the suite."""
    exited = False
    try:
        dev.session.reset_input()
        dev.session.write_line("launch DIAG")
        if b"[M-DIAG]" not in dev.session.read_until(b"[M-DIAG]", 35):
            c.note("DIAG benchmark skipped - app did not start")
            return
        if b"[M-DIAG-MENU]" not in dev.session.read_until(b"[M-DIAG-MENU]", 20):
            c.note("DIAG benchmark skipped - menu did not appear")
            return
        dev.session.read_for(1.5)  # let the list modal open
        dev.session.write_line("7")  # "Benchmark" is the 7th item (0-based 6)
        if b"benchmark" not in dev.session.read_until(b"benchmark", 25):
            c.note("DIAG benchmark selection was not accepted")
            return
        head = dev.session.read_until(b"gfx stats:", 90)
        tail = dev.session.read_for(1.0)
        text = head.decode("utf-8", "replace") + tail
        line = _stat_line(text, "gfx stats:")
        stats = parse_perf_report(line)
        if stats is not None and stats.frames > 0:
            c.check("DIAG benchmark frames > 0", True, stats.as_text())
            c.note("DIAG benchmark gfx stats: " + stats.as_text())
        else:
            c.note("DIAG benchmark gfx stats not parsed: %r" % line)
        if b"[M-DIAG-MENU]" in dev.session.read_until(b"[M-DIAG-MENU]", 30):
            dev.session.write_line("9")  # Exit
            exited = b"[M-DIAG-DONE]" in dev.session.read_until(b"[M-DIAG-DONE]", 30)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.note("DIAG benchmark drive failed: %s" % exc)
    finally:
        if not exited:
            try:
                dev.session.write_line("q")  # list cancel -> menu
                time.sleep(0.3)
                dev.session.write_line("9")  # Exit
                dev.session.read_until(b"[M-DIAG-DONE]", 10)
            except Exception:  # noqa: BLE001
                pass
        try:
            dev.session.write_line("gfx close")  # DIAG may have died mid-bench
            time.sleep(0.3)
        except Exception:  # noqa: BLE001
            pass
        _run(dev, c, "resync after DIAG", "echo p4diagok", timeout=30)


def _cleanup(dev, c):
    """Remove the on-device BMP/TXT artifacts the demos wrote, and make sure
    no full-screen surface (TUI/gfx/app mode) is left owning the worker."""
    # An animation quit that did not complete can leave TUI or the gfx canvas
    # active, and the next BOUNCE/gfx run then refuses ("TUI mode is active").
    # Closing both is harmless when neither is active.
    for cmd in ("draw close", "gfx close"):
        try:
            dev.run(cmd, timeout=15)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    for name in ("BOUNCE.BMP", "SNAKE.BMP", "DIAG_BENCH.BMP", "DIAG.TXT"):
        try:
            dev.run("del %s" % name, timeout=15)
        except PanicError:
            raise
        except Exception:  # noqa: BLE001
            pass
    try:
        dev.run("taskkill bg0", timeout=15)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass
    c.note("cleanup complete; host screenshots kept under %s" % OUT_DIR)


def _screenshot_diff(dev, c):
    """Two *static* gfx canvas frames must look different to the host
    screenshot path.

    A screenshot taken mid-animation is not reliable (the bare streaming
    screenshot runs on the worker and the app's output interleaves), so this
    checks two settled, fully-formed frames instead. Each frame is shown and
    allowed to reach the panel before the capture.
    """
    # Return to a clean foreground first: the DIAG benchmark can leave TUI/app
    # mode active, in which case `gfx init` would be refused and the captures
    # would show the shell (both frames identical).
    _run(dev, c, "draw close", "draw close", timeout=15)
    _run(dev, c, "gfx close", "gfx close", timeout=15)
    init_out = _run(dev, c, "gfx init", "gfx init 320 200", timeout=20)
    if "canvas" not in init_out:
        c.note("gfx canvas could not be opened for the frame-diff check: %r"
               % init_out[-160:])
        # Best effort: close whatever owns the foreground and report.
        _run(dev, c, "draw close", "draw close", timeout=15)
        _run(dev, c, "gfx close", "gfx close", timeout=15)
        return
    # Make the two frames differ over most of the canvas so a whole-screen
    # diff is unambiguous (a thin shape vs a thin shape is within sampling
    # noise at every-4th-pixel).
    _run(dev, c, "gfx frame a",
         "gfx clear 0 & gfx circle 160 100 90 15 fill & gfx show", timeout=20)
    time.sleep(0.6)
    a = dev.screenshot(out_dir=OUT_DIR, name="perf_frame_a")
    _run(dev, c, "gfx frame b",
         "gfx clear 0 & gfx rect 0 0 319 199 15 fill & gfx show", timeout=20)
    time.sleep(0.6)
    b = dev.screenshot(out_dir=OUT_DIR, name="perf_frame_b")
    ratio = p4shot.diff_ratio(a, b)
    c.check("two settled canvas frames differ", ratio > 0.002,
            "diff_ratio=%.4f" % ratio)
    c.note("host screenshot frame diff_ratio=%.4f" % ratio)
    _run(dev, c, "gfx close", "gfx close", timeout=15)


def run(dev, ctx):
    c = Checklist(NAME)
    quick = bool(ctx.get("quick"))

    _deploy(dev, c, FILES)

    # -- BOUNCE: firmware-measured gfx frame pacing ---------------------------
    out = _run(dev, c, "bounce", "launch BOUNCE", timeout=200)
    c.expect("bounce start marker", "[M-BOUNCE]", out)
    c.expect("bounce done marker", "[M-BOUNCE-DONE]", out)

    line = _stat_line(out, "gfx stats:")
    stats = parse_perf_report(line)
    if stats is None:
        c.check("bounce gfx stats parsed", False,
                "no parsable gfx stats line: %r" % line)
    else:
        c.check("bounce frames > 0", stats.frames > 0, stats.as_text())
        c.check("bounce avg interval sane (<200ms)",
                0.0 < stats.interval_ms_avg < 200.0, stats.as_text())
        if not quick:
            c.check("bounce smoothness score >= 40",
                    stats.smoothness_score() >= 40.0, stats.as_text())
        c.note("BOUNCE gfx stats: " + stats.as_text())

    # -- BOUNCE: host-timed [M-BOUNCE-FRAME] markers --------------------------
    if not quick:
        marks = measure_markers(dev.session, "launch BOUNCE",
                                marker="[M-BOUNCE-FRAME]",
                                duration=11.0, target_fps=30)
        _run(dev, c, "shell after marker run", "echo p4perfok", timeout=60)
        c.check("bounce frame markers timed", marks.frames >= 2, marks.as_text())
        c.note("BOUNCE marker pacing: " + marks.as_text())
    else:
        c.note("host marker timing skipped in quick mode")

    # -- SNAKE: tui stats -----------------------------------------------------
    _run(dev, c, "tui stats reset", "tui stats reset", timeout=15)
    dev.session.reset_input()
    dev.session.write_line("launch SNAKE")
    head = dev.session.read_until(b"[M-SNAKE]", 35)
    c.check("snake start marker", b"[M-SNAKE]" in head,
            head[-200:].decode("utf-8", "replace"))
    dev.session.read_for(3.5)  # a few 1 s frames
    # `choice` is a key-wait whose queue is flushed when a new wait starts,
    # and SNAKE auto-advances every second, so a single `q` can be dropped.
    # Send it repeatedly (key + Enter) until the done marker or the deadline.
    tail = ""
    end = time.time() + 30
    while time.time() < end and "[M-SNAKE-DONE]" not in tail:
        dev.session.write_line("q")
        tail += dev.session.read_for(1.2)
    c.check("snake quit marker", "[M-SNAKE-DONE]" in tail,
            tail[-200:])
    # Release the TUI surface if the app did not get to its own `draw close`.
    _run(dev, c, "snake tui cleanup", "draw close", timeout=15)

    sout = _run(dev, c, "tui stats", "tui stats", timeout=15)
    sline = _stat_line(sout, "tui stats:")
    sstats = parse_perf_report(sline)
    if sstats is None:
        c.check("snake tui stats parsed", False, "line=%r" % sline)
    else:
        c.check("snake tui frames > 0", sstats.frames > 0, sstats.as_text())
        c.note("SNAKE tui stats: " + sstats.as_text())

    # -- DIAG benchmark (best effort) -----------------------------------------
    if not quick:
        _diag_benchmark(dev, c)
    else:
        c.note("DIAG benchmark skipped in quick mode; BOUNCE covers gfx stats")

    # -- host screenshot of an animation frame changes ------------------------
    _screenshot_diff(dev, c)

    _cleanup(dev, c)
    return c
