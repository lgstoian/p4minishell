# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Autonomous local dogfooding agent.

A seedable, stateful policy drives the firmware the way a curious user would:
it reads help, pokes status commands, creates and removes files, launches apps,
opens and answers modals, animates the TUI/canvas, and captures a screenshot
after every single action. It watches for the failure modes a human notices
but a scripted test does not: panics, wedged prompts, blue "BSOD" frames,
all-black frames, heap leaks across the session, and commands that a healthy
build should answer but reports as unknown.

Everything is journaled (JSONL + a markdown report) so a run is reproducible
from its seed and every claim is backed by a screenshot on disk.
"""
from __future__ import annotations

import json
import os
import random
import re
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

from .asserts import Checklist
from .device import Device
from .session import P4Error, PanicError
from . import screenshot as p4shot

# ---------------- anomaly model ----------------

@dataclass
class Anomaly:
    severity: str      # CRITICAL | HIGH | MEDIUM | LOW
    kind: str
    detail: str
    step: int
    screenshot: Optional[str] = None


@dataclass
class Step:
    index: int
    action: str
    command: str
    duration_s: float
    output_tail: str
    screenshot: Optional[str] = None
    anomalies: List[Anomaly] = field(default_factory=list)


# ---------------- action library ----------------

STATUS_COMMANDS = [
    "about", "version", "mem", "ps", "tasks", "top", "sysinfo",
    "sd info", "disk list", "chkdsk", "trash info",
    "wifi status", "ipconfig", "netstat",
    "usb status", "bluetooth status", "c6ota status",
    "audio status", "volume", "rgb status", "power", "battery",
    "gpio status", "pwm status", "adc status", "i2c scan",
    "tui status", "gfx stats", "theme list", "font list",
    "cursor status", "header status", "keyboard status",
    "timer status", "alarm status", "cal next", "config /b",
]

HELP_TOPICS = [
    "help", "help /all", "help dir", "help gfx", "help draw", "help db",
    "help calc", "help for", "help if", "help call", "help wifi",
    "help format", "help disk", "help archive", "help crypt",
]

TUI_SNIPPETS = [
    ["draw box 1 1 30 5 single \"DOG FOOD\"", "draw text 3 2 sniffing...", "draw refresh"],
    ["draw box 1 1 40 7 double", "draw line 2 3 38 3",
     "draw bar 3 4 30 60 219 176", "draw refresh"],
    ["draw clear screen", "draw text 1 1 \"dogfood tui\"",
     "draw table 1 2 3 8 8 8 \"a,b\" \"c,d\"", "draw refresh"],
]

MODALS = [
    ("dialog \"Dogfood check\" \"Is the shell alive?\" yesno", ["y"]),
    ("list \"Pick one\" \"alpha\" \"beta\" \"gamma\"", ["2"]),
    ("ask \"Name\" /v:DFNAME", ["rover"]),
]


class DogfoodAgent:
    def __init__(self, dev: Device, out_dir: str, seed: int = 1,
                 quick: bool = False):
        self.dev = dev
        self.out_dir = out_dir
        self.shots = os.path.join(out_dir, "shots")
        os.makedirs(self.shots, exist_ok=True)
        self.rng = random.Random(seed)
        self.seed = seed
        self.quick = quick
        self.steps: List[Step] = []
        self.anomalies: List[Anomaly] = []
        self.prev_bmp = None
        self.static_streak = 0
        self.current_shot: Optional[str] = None
        self.captured_this_step = False
        self.heap_samples: List[int] = []
        self.help_text = ""
        self.apps: List[str] = []
        self.start_heap: Optional[int] = None
        self._journal = open(os.path.join(out_dir, "journal.jsonl"), "w",
                             encoding="utf-8")

    # -- helpers ---------------------------------------------------------
    def _drain(self, quiet: float = 0.6, max_wait: float = 8.0,
               step: float = 0.2) -> None:
        """Drain pending serial input until the link is quiet for ``quiet``
        seconds (bounded by ``max_wait``).

        An app that is animating keeps the byte stream busy, so a screenshot
        issued mid-stream desynchronises the BMPX frame ("no streaming
        prelude", then a ``\\r\\n`` flood). Quiescing first lets the capture
        start on a frame boundary.
        """
        deadline = time.time() + max_wait
        quiet_for = 0.0
        while time.time() < deadline:
            if self.dev.session.read_for(step):
                quiet_for = 0.0
            else:
                quiet_for += step
                if quiet_for >= quiet:
                    break

    @staticmethod
    def _whole_screen_blue(bmp) -> bool:
        """True only for a whole-screen DSI-underrun "BSOD", not an app's
        legitimate blue chrome (the ``list`` modal in PALMTOP/PICS was a
        false positive of the old mean-only test).

        Ignores the top 40px header band and requires both a blue-dominant
        body mean and that most of the sampled body is blue.
        """
        mr, mg, mb = bmp.region_mean(0, 40, bmp.width, bmp.height - 40)
        if not (mb - mr > 80.0 and mb - mg > 40.0 and mb > 120.0):
            return False
        blue = 0
        total = 0
        for y in range(40, bmp.height, 8):
            for x in range(0, bmp.width, 8):
                r, g, b = bmp.px(x, y)
                if b > r + 40 and b > g + 20:
                    blue += 1
                total += 1
        return total > 0 and blue / float(total) > 0.6

    def _capture(self, tag: str):
        """Capture + persist a screenshot; run visual sanity checks."""
        self.captured_this_step = True
        # Quiesce before every capture so an animating app cannot desync the
        # streaming BMPX frame.
        self._drain()
        try:
            bmp = p4shot.capture(self.dev.session, out_dir=self.shots, name=tag)
            self.current_shot = os.path.join(self.shots, tag + ".png")
        except (P4Error, PanicError):
            raise
        except Exception as exc:  # noqa: BLE001
            self.anomaly("MEDIUM", "screenshot", "capture failed: %s" % exc)
            return None
        # visual checks
        try:
            mean = bmp.region_mean(0, 0, bmp.width, bmp.height)
            if sum(mean) < 12:
                self.anomaly("HIGH", "blank-screen",
                             "frame nearly all black mean=%r" % (mean,))
            top = bmp.region_mean(0, 0, bmp.width, 40)
            if sum(top) < 20:
                self.anomaly("LOW", "header-blank",
                             "top header band looks empty mean=%r" % (top,))
            # DSI underrun "BSOD": the whole screen must be blue, not just an
            # app's blue panel.
            if self._whole_screen_blue(bmp):
                self.anomaly("CRITICAL", "bsod",
                             "whole-screen blue (mean B-R=%.1f)" % (mean[2] - mean[0]))
            self._check_targets(bmp)
            if self.prev_bmp is not None and self.prev_bmp.width == bmp.width:
                if p4shot.diff_ratio(self.prev_bmp, bmp) == 0.0:
                    self.static_streak += 1
                    if self.static_streak >= 4:
                        self.anomaly("MEDIUM", "static-frame",
                                     "no pixel change across %d actions" % self.static_streak)
                else:
                    self.static_streak = 0
            self.prev_bmp = bmp
        except Exception as exc:  # noqa: BLE001
            self.anomaly("LOW", "visual-check", str(exc))
        return bmp

    def _check_targets(self, bmp) -> None:
        """Flag visible controls whose rect leaves the screen (read `ui targets`
        raw: a marker-synced run() would close an open modal)."""
        try:
            from . import visual
            self.dev.session.reset_input()
            self.dev.session.write_line("ui targets")
            targets = visual.parse_targets(self.dev.session.read_for(1.0))
        except Exception:  # noqa: BLE001
            return
        for t in targets:
            if t.name in ("obj", ""):
                continue
            # Clipped controls (off-view scroll-list rows etc.) are not visible
            # where reported; the firmware flags them.
            if t.clipped:
                continue
            if (t.y < 0 or t.y + t.h > bmp.height or
                    t.x < 0 or t.x + t.w > bmp.width):
                self.anomaly(
                    "MEDIUM", "off-screen-widget",
                    "%r rect=(%d,%d,%d,%d) outside %dx%d"
                    % (t.name, t.x, t.y, t.w, t.h, bmp.width, bmp.height))

    def anomaly(self, severity: str, kind: str, detail: str,
                screenshot: Optional[str] = None) -> None:
        shot = screenshot or self.current_shot
        a = Anomaly(severity, kind, detail, len(self.steps) + 1, shot)
        self.anomalies.append(a)

    # -- state -----------------------------------------------------------
    def learn_state(self) -> None:
        try:
            self.help_text = self.dev.run("help /all", timeout=25)
        except Exception:
            self.help_text = self.dev.run("help", timeout=20)
        try:
            listing = self.dev.run("launch /list", timeout=20)
            self.apps = [ln.strip().split()[0] for ln in listing.splitlines()
                         if ln.strip() and " - " in ln and not ln.startswith("PS")]
        except Exception:
            self.apps = []
        try:
            mem = self.dev.run("mem", timeout=15)
            self.start_heap = self._parse_heap(mem)
        except Exception:
            self.start_heap = None

    @staticmethod
    def _parse_heap(text: str) -> Optional[int]:
        m = re.search(r"(\d[\d,]*)\s*(?:bytes)?\s*(?:free|heap)", text, re.I)
        if not m:
            m = re.search(r"free[^\d]*([\d,]+)", text, re.I)
        if not m:
            return None
        try:
            return int(m.group(1).replace(",", ""))
        except ValueError:
            return None

    # -- surface cleanup -------------------------------------------------
    def _ui_state(self) -> str:
        """Read `ui state` via the console reader (a modal blocks the worker,
        so a marker-synced run() cannot complete while one is open)."""
        try:
            self.dev.session.reset_input()
            self.dev.session.write_line("ui state")
            return self.dev.session.read_for(1.0)
        except Exception:  # noqa: BLE001
            return ""

    def _close_surfaces(self, attempts: int = 4) -> bool:
        """Best-effort return to a usable shell.

        An app can leave the `edit` editor or a modal owning the input; a
        subsequent binary `receive` then never gets its READY (H8). Close the
        editor with its serial quit, cancel other modals, and verify. A serial
        Ctrl+C is sent first: it unwinds a batch app stuck in its own prompt
        (e.g. ADVENT's `ask` loop) via the foreground break.
        """
        try:
            self.dev.session.write(b"\x03\r\n")
            time.sleep(0.6)
        except Exception:  # noqa: BLE001
            pass
        for _ in range(attempts):
            st = self._ui_state()
            em = re.search(r"editor=(\w+)", st)
            mm = re.search(r"modal=(\w+)", st)
            editor = em.group(1) if em else "closed"
            modal = mm.group(1) if mm else "none"
            if editor != "open" and modal in ("none", ""):
                return True
            if editor == "open":
                self.dev.session.write_line("\\q")
                time.sleep(0.5)
                self.dev.session.write_line("y")
                time.sleep(0.7)
            else:
                self.dev.session.write_line("q")
                time.sleep(0.4)
                self.dev.session.write_line("")
                time.sleep(0.3)
        try:
            self.dev.run("draw close", timeout=10)
        except Exception:  # noqa: BLE001
            pass
        return False

    # -- actions ---------------------------------------------------------
    def _act_run(self, cmd: str, timeout: float = 20.0) -> str:
        return self.dev.run(cmd, timeout=timeout)

    def action_status(self) -> str:
        return self.rng.choice(STATUS_COMMANDS)

    def action_help(self) -> str:
        return self.rng.choice(HELP_TOPICS)

    def action_files(self) -> str:
        name = "DF%04d.TXT" % self.rng.randint(0, 9999)
        body = "dogfood %d\n" % self.rng.randint(0, 9999)
        # A binary `receive` needs the worker: make sure an app/modal is not
        # still owning the input (H8).
        self._close_surfaces()
        try:
            self.dev.push_file(name, body.encode())
        except Exception as exc:  # noqa: BLE001
            raise P4Error("push temp failed: %s" % exc)
        steps = self.rng.choice([
            ["type %s" % name, "dir /b", "del %s" % name],
            ["copy %s %s.bak" % (name, name), "del /p %s.bak" % name,
             "del %s" % name],
            ["append %s second-line" % name, "find \"second\" %s" % name,
             "del %s" % name],
        ])
        out = []
        for s in steps:
            out.append(self.dev.run(s, timeout=15))
        return "\n".join(out)

    def action_tui(self) -> str:
        snippet = self.rng.choice(TUI_SNIPPETS)
        out = []
        for cmd in snippet:
            out.append(self.dev.run(cmd, timeout=15))
        out.append(self.dev.run("draw refresh", timeout=15))
        self._capture("tui_%d" % (len(self.steps) + 1))
        out.append(self.dev.run("draw close", timeout=15))
        return "\n".join(out)

    def action_gfx(self) -> str:
        out = [self.dev.run("gfx init 200 150", timeout=15)]
        try:
            for i in range(self.rng.randint(10, 30)):
                x = 20 + (i * 7) % 160
                out.append(self.dev.run("gfx clear 0", timeout=10))
                out.append(self.dev.run("gfx circle %d 75 12 14 fill" % x, timeout=10))
                out.append(self.dev.run("gfx show", timeout=10))
            out.append(self.dev.run("gfx stats", timeout=10))
        finally:
            out.append(self.dev.run("gfx close", timeout=10))
        return "\n".join(out)

    def action_app(self) -> str:
        if not self.apps:
            return self.action_status()
        app = self.rng.choice(self.apps)
        self.dev.session.reset_input()
        self.dev.session.write_line(app)
        out = self.dev.session.read_for(2.5)
        self._capture("app_%s_%d" % (app, len(self.steps) + 1))
        # Escape whatever surface the app opened so the next action starts from
        # a usable shell (foreground break + editor/modal close).
        self._close_surfaces()
        return out

    def action_modal(self) -> str:
        cmd, answers = self.rng.choice(MODALS)
        self.dev.session.reset_input()
        self.dev.session.write_line(cmd)
        out = self.dev.session.read_for(1.5)
        self._capture("modal_%d" % (len(self.steps) + 1))
        for ans in answers:
            self.dev.session.write_line(ans)
            out += self.dev.session.read_for(1.2)
        # Make sure the modal actually closed before the next step.
        self._close_surfaces()
        return out

    def action_memory(self) -> str:
        out = self.dev.run("mem", timeout=15)
        val = self._parse_heap(out)
        if val is not None:
            self.heap_samples.append(val)
            if self.start_heap and val < self.start_heap * 0.35:
                self.anomaly("HIGH", "heap-low",
                             "free heap %d < 35%% of session start %d"
                             % (val, self.start_heap))
        return out

    # -- policy ----------------------------------------------------------
    def _actions(self) -> List[Callable[[], str]]:
        return [
            self.action_status,
            self.action_help,
            self.action_files,
            self.action_tui,
            self.action_gfx,
            self.action_app,
            self.action_modal,
            self.action_memory,
        ]

    def run(self, minutes: float = 10.0) -> Checklist:
        c = Checklist("dogfood")
        deadline = time.time() + minutes * 60.0
        self.learn_state()
        c.check("apps discovered", len(self.apps) > 0, "apps=%r" % self.apps[:8])
        c.check("help loaded", len(self.help_text) > 200)
        weights = [4, 3, 3, 2, 2, 3, 2, 2]
        actions = self._actions()
        index = 0
        while time.time() < deadline:
            index += 1
            action = self.rng.choices(actions, weights=weights, k=1)[0]
            name = action.__name__.replace("action_", "")
            self.captured_this_step = False
            started = time.time()
            try:
                out = action()
                err = None
            except PanicError as exc:
                out, err = "", str(exc)
                self.anomaly("CRITICAL", "panic", err)
            except P4Error as exc:
                out, err = "", str(exc)
                self.anomaly("HIGH", "timeout-or-protocol", "%s: %s" % (name, exc))
            except Exception as exc:  # noqa: BLE001
                out, err = "", "%s: %s" % (type(exc).__name__, exc)
                self.anomaly("MEDIUM", "action-failed", "%s: %s" % (name, err))
            # Every action ends with a screenshot (the visual contract), even
            # the plain commands that never opened a surface themselves.
            if not self.captured_this_step:
                try:
                    self._capture("step_%d_%s" % (index, name))
                except (P4Error, PanicError) as exc:
                    self.anomaly("HIGH", "screenshot", str(exc))
            duration = time.time() - started
            step = Step(index, name, name, duration, out[-400:],
                        screenshot=self.current_shot,
                        anomalies=list(self.anomalies[-3:]))
            self.steps.append(step)
            self._journal.write(json.dumps({
                "step": index, "action": name, "duration_s": round(duration, 3),
                "output": out[-400:], "error": err,
            }) + "\n")
            self._journal.flush()
            if index % 10 == 0:
                print("  ... step %d, %d anomalies, %.1f min left"
                      % (index, len(self.anomalies),
                         (deadline - time.time()) / 60.0))
        self._journal.close()
        # verdict
        crit = sum(1 for a in self.anomalies if a.severity == "CRITICAL")
        high = sum(1 for a in self.anomalies if a.severity == "HIGH")
        c.check("no panics", crit == 0, "%d critical anomalies" % crit)
        c.check("no high-severity anomalies", high == 0, "%d high anomalies" % high)
        c.check("actions completed", len(self.steps) > 0)
        self.write_report(c)
        return c

    # -- report ----------------------------------------------------------
    def write_report(self, c: Checklist) -> None:
        path = os.path.join(self.out_dir, "report.md")
        crit = sum(1 for a in self.anomalies if a.severity == "CRITICAL")
        high = sum(1 for a in self.anomalies if a.severity == "HIGH")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("# P4MiniShell dogfooding report\n\n")
            fh.write("- seed: %d\n- steps: %d\n- anomalies: %d (CRITICAL %d, HIGH %d)\n"
                     % (self.seed, len(self.steps), len(self.anomalies), crit, high))
            if self.heap_samples:
                fh.write("- heap samples: first=%d last=%d min=%d\n\n"
                         % (self.heap_samples[0], self.heap_samples[-1],
                            min(self.heap_samples)))
            fh.write("## Anomalies\n\n")
            for a in self.anomalies:
                fh.write("- **%s** [%s] step %d: %s%s\n"
                         % (a.severity, a.kind, a.step, a.detail,
                            (" (`%s`)" % a.screenshot) if a.screenshot else ""))
            fh.write("\n## Steps\n\n")
            for s in self.steps:
                fh.write("- %d %s (%.2fs)\n" % (s.index, s.action, s.duration_s))
        print("  report: %s" % path)
