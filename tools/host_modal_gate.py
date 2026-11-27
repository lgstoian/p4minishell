#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_modal_gate.py - hardware-free modal state-transition gate.

The gate generates a C harness from ``tools/host/fixtures/modal/transitions.json``
and compiles it together with the REAL pure translation unit the firmware
surfaces call (``components/modal/modal_args.c``). Python never re-implements
the focus / progress / session state machine: the checks run against shipped
code, natively, with no board, LVGL, FreeRTOS or display.

A pass is a pre-gate only. It says nothing about on-target behaviour;
``harness.md`` §0 still governs hardware sign-off.

Usage:
    python tools/host_modal_gate.py              # --check (default)
    python tools/host_modal_gate.py --check
    python tools/host_modal_gate.py --fixture PATH
    python tools/host_modal_gate.py --cc gcc
    python tools/host_modal_gate.py --self-test  # red canary for the gate
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Tuple

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tools" / "host" / "fixtures" / "modal" / "transitions.json"
MODAL_DIR = ROOT / "components" / "modal"
PURE_SRC = MODAL_DIR / "modal_args.c"
# The pure switcher renderer and swipe predicate are header-only in tui_frame.h;
# they are compiled into the SAME harness so the gate checks shipped code, not a
# host-only copy. Header deps are pure headers (gfx, ansi, esp_err stub).
EXTRA_INCLUDES = [
    ROOT / "components" / "tui",
    ROOT / "components" / "ansi",
    ROOT / "components" / "gfx",
    ROOT / "tools" / "host" / "fixtures" / "tui" / "stubs",
    ROOT,
]
MIN_CHECKS = 20
RUN_TIMEOUT = 60

BEGIN = "modal.gate.begin"
END = "modal.gate.end"
SUMMARY = "modal.gate.summary"
CHECK_RE = re.compile(r"^modal\.check\s+([A-Za-z0-9_.]+)\s+(ok|FAIL)$")
SUMMARY_RE = re.compile(r"^modal\.gate\.summary\s+checks=(\d+)\s+fail=(\d+)$")

KEYS = {
    "tab": "MODAL_KEY_TAB",
    "up": "MODAL_KEY_UP", "down": "MODAL_KEY_DOWN",
    "left": "MODAL_KEY_LEFT", "right": "MODAL_KEY_RIGHT",
    "home": "MODAL_KEY_HOME", "end": "MODAL_KEY_END",
    "page_up": "MODAL_KEY_PAGE_UP", "page_down": "MODAL_KEY_PAGE_DOWN",
    "enter": "MODAL_KEY_ENTER",
    "other": "0x99u",
}
STATES = {"fore": "MODAL_SESSION_FORE", "back": "MODAL_SESSION_BACK"}
# CP437 box glyphs as C string literals (UTF-8 escapes keep the generated
# harness ASCII-only). The names match the fixture's "glyph" field.
GLYPHS = {
    "tl": '"\\xE2\\x94\\x8C"', "tr": '"\\xE2\\x94\\x90"',
    "bl": '"\\xE2\\x94\\x94"', "br": '"\\xE2\\x94\\x98"',
    "h": '"\\xE2\\x94\\x80"', "v": '"\\xE2\\x94\\x82"',
}
MODS = {
    "left_alt": "MODAL_MOD_LEFT_ALT", "right_alt": "MODAL_MOD_RIGHT_ALT",
    "ctrl": "0x01u", "shift": "0x02u", "none": "0u",
}


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


def cstr(value: str) -> str:
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def ident(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", value)


def load_fixture(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        raise GateError("fixture is missing: %s" % path)
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError("fixture unreadable: %s" % exc)
    tag = data.get("tag")
    if not isinstance(tag, str) or not tag:
        raise GateError("fixture has no tag")
    for group in ("focus", "progress", "sessions", "input", "gestures",
                  "switch", "switcher"):
        if not isinstance(data.get(group), list) or not data[group]:
            raise GateError("fixture group %r is missing or vacuous" % group)
    min_cases = {"focus": 6, "progress": 4, "sessions": 2, "input": 4,
                 "gestures": 4, "switch": 2, "switcher": 2}
    for group, minimum in min_cases.items():
        if len(data[group]) < minimum:
            raise GateError("fixture group %r has too few cases (%d < %d)"
                            % (group, len(data[group]), minimum))
    return data


def _check(name: str, condition: str) -> str:
    return '    CHECK(%s, %s);' % (cstr(name), condition)


def render_focus(cases: List[dict]) -> List[str]:
    out = []
    for i, case in enumerate(cases):
        for field in ("name", "key", "next", "count", "current",
                      "page_step", "changed"):
            if field not in case:
                raise GateError("focus case %d lacks %r" % (i, field))
        key = case["key"]
        if key not in KEYS:
            raise GateError("focus case %d has unknown key %r" % (i, key))
        stem = "focus.%s" % ident(case["name"])
        out.extend([
            "    {",
            # Untouched-out means focus stays where it was.
            "        int out_%d = %d;" % (i, int(case["current"])),
            "        bool changed_%d = modal_focus_move(%s, %d, %d, %d, &out_%d);"
            % (i, KEYS[key], int(case["count"]), int(case["current"]),
               int(case["page_step"]), i),
            _check(stem + ".changed", "changed_%d == %s"
                   % (i, "true" if case["changed"] else "false")),
            _check(stem + ".next", "out_%d == %d" % (i, int(case["next"]))),
            "    }",
        ])
    return out


def render_progress(cases: List[dict]) -> List[str]:
    out = []
    for i, case in enumerate(cases):
        for field in ("name", "done", "total", "pct"):
            if field not in case:
                raise GateError("progress case %d lacks %r" % (i, field))
        out.append(_check("progress.%s" % ident(case["name"]),
                          "modal_progress_percent(%du, %du) == %du"
                          % (int(case["done"]), int(case["total"]),
                             int(case["pct"]))))
    return out


def render_sessions(scenarios: List[dict]) -> List[str]:
    out = []
    for sidx, scenario in enumerate(scenarios):
        name = scenario.get("name")
        ops = scenario.get("ops")
        if not isinstance(name, str) or not isinstance(ops, list) or not ops:
            raise GateError("session scenario %d is malformed" % sidx)
        tag = ident(name)
        out.extend([
            "    /* scenario: %s */" % name,
            "    {",
            "        modal_session_stack_t st_%d;" % sidx,
            "        modal_session_stack_init(&st_%d);" % sidx,
        ])
        for oidx, op in enumerate(ops):
            kind = op.get("op")
            st = "st_%d" % sidx
            label = "%s.op%d_%s" % (tag, oidx, ident(str(kind)))
            if kind == "push":
                out.extend([
                    "        bool ok_%d_%d = modal_session_stack_push(&%s, %s);"
                    % (sidx, oidx, st, cstr(op.get("name", ""))),
                    _check(label, "ok_%d_%d == %s"
                           % (sidx, oidx, "true" if op.get("expect_ok") else "false")),
                ])
            elif kind in ("pop", "suspend", "resume"):
                fn = "modal_session_stack_" + kind
                out.extend([
                    "        bool ok_%d_%d = %s(&%s);" % (sidx, oidx, fn, st),
                    _check(label, "ok_%d_%d == %s"
                           % (sidx, oidx, "true" if op.get("expect_ok") else "false")),
                ])
            elif kind == "switch":
                out.extend([
                    "        bool ok_%d_%d = modal_session_stack_switch(&%s, %s);"
                    % (sidx, oidx, st, cstr(op.get("name", ""))),
                    _check(label, "ok_%d_%d == %s"
                           % (sidx, oidx, "true" if op.get("expect_ok") else "false")),
                ])
            elif kind == "expect_depth":
                out.append(_check(label, "modal_session_stack_depth(&%s) == %d"
                                  % (st, int(op.get("value", -1)))))
            elif kind == "expect_empty":
                out.append(_check(label, "modal_session_stack_empty(&%s) == %s"
                                  % (st, "true" if op.get("value") else "false")))
            elif kind == "expect_full":
                out.append(_check(label, "modal_session_stack_full(&%s) == %s"
                                  % (st, "true" if op.get("value") else "false")))
            elif kind in ("expect_top", "expect_name"):
                from_top = 0 if kind == "expect_top" else int(op.get("from_top", 0))
                out.append(_check(label, "name_eq(modal_session_stack_name(&%s, %d), %s)"
                                  % (st, from_top, cstr(op.get("value", "")))))
            elif kind == "expect_find":
                out.append(_check(label, "modal_session_stack_find(&%s, %s) == %d"
                                  % (st, cstr(op.get("name", "")), int(op.get("value", -99)))))
            elif kind == "expect_state":
                from_top = int(op.get("from_top", 0))
                state = op.get("value")
                if state not in STATES:
                    raise GateError("bad session state %r" % state)
                cond = ("(%d >= 0 && %d < modal_session_stack_depth(&%s) && "
                        "st_%d.slot[modal_session_stack_depth(&%s) - 1 - %d].state == %s)"
                        % (from_top, from_top, st, sidx, st, from_top, STATES[state]))
                out.append(_check(label, cond))
            elif kind == "set_focus":
                out.extend([
                    "        bool ok_%d_%d = modal_session_stack_set_focus(&%s, %d, %d);"
                    % (sidx, oidx, st, int(op.get("from_top", 0)),
                       int(op.get("value", -1))),
                    _check(label, "ok_%d_%d == %s"
                           % (sidx, oidx, "true" if op.get("expect_ok") else "false")),
                ])
            elif kind == "expect_focus":
                out.append(_check(label, "modal_session_stack_focus(&%s, %d) == %d"
                                  % (st, int(op.get("from_top", 0)),
                                     int(op.get("value", -1)))))
            else:
                raise GateError("unknown session op %r" % kind)
        out.append("    }")
    return out


def render_input(cases: List[dict]) -> List[str]:
    """Mock HID byte pairs against the real Alt+Tab predicate."""
    out: List[str] = []
    for i, case in enumerate(cases):
        for field in ("name", "key", "mods", "switches"):
            if field not in case:
                raise GateError("input case %d lacks %r" % (i, field))
        key = case["key"]
        if key not in KEYS:
            raise GateError("input case %d has unknown key %r" % (i, key))
        out.append(_check("input.%s" % ident(case["name"]),
                          "modal_alt_tab(%s, %du) == %s"
                          % (KEYS[key], int(case["mods"]),
                             "true" if case["switches"] else "false")))
    return out


def render_gestures(cases: List[dict]) -> List[str]:
    """Mock pointer vectors against the same pure predicate the tui uses."""
    out: List[str] = []
    for i, case in enumerate(cases):
        for field in ("name", "x0", "y0", "x1", "y1", "w", "h", "swipe"):
            if field not in case:
                raise GateError("gesture case %d lacks %r" % (i, field))
        out.append(_check("gesture.%s" % ident(case["name"]),
                          "tui_edge_swipe_bottom_left(%d, %d, %d, %d, %d, %d) == %s"
                          % (int(case["x0"]), int(case["y0"]),
                             int(case["x1"]), int(case["y1"]),
                             int(case["w"]), int(case["h"]),
                             "true" if case["swipe"] else "false")))
    return out


def render_switch(scenarios: List[dict]) -> List[str]:
    """Cycle order on the REAL stack: no lost session, no frozen slot."""
    out: List[str] = []
    for sidx, scenario in enumerate(scenarios):
        name = scenario.get("name")
        sessions = scenario.get("sessions")
        order = scenario.get("expect_order")
        if (not isinstance(name, str) or not isinstance(sessions, list) or
                not sessions or not isinstance(order, list) or not order):
            raise GateError("switch scenario %d is malformed" % sidx)
        if len(order) != len(sessions):
            raise GateError("switch scenario %d order/count mismatch" % sidx)
        tag = ident(name)
        st = "sw_%d" % sidx
        out.extend([
            "    /* switch scenario: %s */" % name,
            "    {",
            "        modal_session_stack_t %s;" % st,
            "        modal_session_stack_init(&%s);" % st,
        ])
        for idx, session in enumerate(sessions):
            out.append("        bool push_%d_%d = modal_session_stack_push(&%s, %s);"
                       % (sidx, idx, st, cstr(session)))
            out.append(_check("%s.push%d" % (tag, idx),
                              "push_%d_%d == true" % (sidx, idx)))
        out.extend([
            "        const char *next_%d = modal_session_next_name(&%s, %d);"
            % (sidx, st, int(scenario.get("step", 1))),
            _check("%s.next" % tag,
                   "name_eq(next_%d, %s)" % (sidx, cstr(scenario.get("expect_next", "")))),
            "        bool switched_%d = modal_session_stack_switch(&%s, next_%d);"
            % (sidx, st, sidx),
            _check("%s.switched" % tag, "switched_%d == true" % sidx),
            # A fake switch that leaves the stack untouched fails these order
            # checks; depth + find prove no session was lost in the rotation.
            _check("%s.depth" % tag,
                   "modal_session_stack_depth(&%s) == %d" % (st, len(sessions))),
        ])
        for pos, expected in enumerate(order):
            out.append(_check("%s.order%d" % (tag, pos),
                              "name_eq(modal_session_stack_name(&%s, %d), %s)"
                              % (st, pos, cstr(expected))))
        for session in sessions:
            out.append(_check("%s.keep_%s" % (tag, ident(session)),
                              "modal_session_stack_find(&%s, %s) >= 0"
                              % (st, cstr(session))))
        out.append("    }")
    return out


def render_switcher(cases: List[dict]) -> List[str]:
    """Check cells produced by the ONE tui frame renderer's switcher scene."""
    out: List[str] = []
    for sidx, case in enumerate(cases):
        name = case.get("name")
        names = case.get("names")
        cells = case.get("cells")
        if (not isinstance(name, str) or not isinstance(names, list) or
                not isinstance(cells, list) or not cells):
            raise GateError("switcher case %d is malformed" % sidx)
        tag = ident(name)
        out.extend([
            "    /* switcher scene: %s */" % name,
            "    {",
            "        tui_cell_t present_%d[80 * 25];" % sidx,
            "        tui_cell_t work_%d[80 * 25];" % sidx,
            "        tui_frame_t frame_%d;" % sidx,
            "        const char *nm_%d[%d];" % (sidx, max(1, len(names))),
        ])
        for idx, label in enumerate(names):
            out.append("        nm_%d[%d] = %s;" % (sidx, idx, cstr(label)))
        out.extend([
            "        memset(present_%d, 0, sizeof(present_%d));" % (sidx, sidx),
            "        memset(work_%d, 0, sizeof(work_%d));" % (sidx, sidx),
            "        tui_frame_bind(&frame_%d, 80, 25, present_%d, work_%d);"
            % (sidx, sidx, sidx),
            "        tui_frame_draw_app_switcher(&frame_%d, %s, %d, %d);"
            % (sidx, "nm_%d" % sidx if names else "NULL", len(names),
               int(case.get("focus", 0))),
        ])
        for cidx, cell in enumerate(cells):
            for field in ("col", "row", "fg", "bg"):
                if field not in cell:
                    raise GateError("switcher %s cell %d lacks %r"
                                    % (name, cidx, field))
            if "glyph" in cell:
                glyph = cell["glyph"]
                if glyph not in GLYPHS:
                    raise GateError("switcher %s cell %d has unknown glyph %r"
                                    % (name, cidx, glyph))
                want = GLYPHS[glyph]
            elif "text" in cell:
                want = cstr(cell["text"])
            else:
                raise GateError("switcher %s cell %d has no glyph/text"
                                % (name, cidx))
            out.append(_check("%s.cell%d" % (tag, cidx),
                              "cell_eq_(tui_frame_cell(&frame_%d, %d, %d), "
                              "%s, %d, %d)"
                              % (sidx, int(cell["col"]), int(cell["row"]),
                                 want, int(cell["fg"]), int(cell["bg"]))))
        out.append("    }")
    return out


def render_harness(data: Dict[str, Any]) -> str:
    body: List[str] = []
    body.extend(render_focus(data["focus"]))
    body.extend(render_progress(data["progress"]))
    body.extend(render_sessions(data["sessions"]))
    body.extend(render_input(data["input"]))
    body.extend(render_gestures(data["gestures"]))
    body.extend(render_switch(data["switch"]))
    body.extend(render_switcher(data["switcher"]))
    return "\n".join([
        "/* GENERATED by tools/host_modal_gate.py - do not edit. */",
        '#include "modal_surf.h"',
        '#include "tui_frame.h"',
        "#include <stdio.h>",
        "#include <string.h>",
        "",
        "static int s_checks = 0;",
        "static int s_failures = 0;",
        '#define CHECK(name, cond) do { s_checks++; if (cond) {'
        ' printf("modal.check %s ok\\n", (name)); } else { s_failures++;'
        ' printf("modal.check %s FAIL\\n", (name)); } } while (0)',
        "",
        "static bool name_eq(const char *got, const char *want)",
        "{",
        "    if (got == NULL) return want[0] == '\\0';",
        "    return strcmp(got, want) == 0;",
        "}",
        "",
        "static bool cell_eq_(const tui_cell_t *c, const char *utf8,",
        "                     uint8_t fg, uint8_t bg)",
        "{",
        "    return c != NULL && strcmp(c->utf8, utf8) == 0 &&",
        "           c->fg == fg && c->bg == bg;",
        "}",
        "",
        "int main(void)",
        "{",
        '    printf("' + BEGIN + ' tag=' + data['tag'] + '\\n");',
    ] + body + [
        '    printf("' + SUMMARY + ' checks=%d fail=%d\\n", s_checks, s_failures);',
        '    printf("' + END + '\\n");',
        "    return s_failures == 0 ? 0 : 1;",
        "}",
        "",
    ])


def parse_output(raw: str, returncode: int) -> Tuple[int, int]:
    text = raw.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.strip() for line in text.split("\n")]
    begins = [l for l in lines if l.startswith(BEGIN + " ")]
    ends = [l for l in lines if l == END]
    summaries = [SUMMARY_RE.match(l) for l in lines if l.startswith(SUMMARY + " ")]
    summaries = [m for m in summaries if m]
    checks = [CHECK_RE.match(l) for l in lines if l.startswith("modal.check ")]
    checks = [m for m in checks if m]

    if len(begins) != 1:
        raise GateError("expected exactly one begin marker, got %d" % len(begins))
    if len(ends) != 1:
        raise GateError("expected exactly one end marker, got %d" % len(ends))
    if len(summaries) != 1:
        raise GateError("expected exactly one summary marker, got %d" % len(summaries))
    checks_count = int(summaries[0].group(1))
    fail_count = int(summaries[0].group(2))

    names = [m.group(1) for m in checks]
    if len(names) != len(set(names)):
        raise GateError("duplicate modal.check name in harness output")
    fails = [m.group(1) for m in checks if m.group(2) == "FAIL"]
    if checks_count != len(checks):
        raise GateError("summary checks=%d but %d check lines"
                        % (checks_count, len(checks)))
    if fail_count != len(fails):
        raise GateError("summary fail=%d but %d FAIL lines" % (fail_count, len(fails)))
    if len(checks) < MIN_CHECKS:
        raise GateError("vacuous harness: %d checks, need >= %d"
                        % (len(checks), MIN_CHECKS))
    if returncode != 0 or fails:
        raise GateError("harness red: rc=%d, failing checks: %s"
                        % (returncode, ", ".join(fails[:6]) or "(summary only)"))
    return checks_count, len(fails)


def audit_wiring() -> List[str]:
    """Reject a tree where the descriptors or verbs silently disappeared.

    This is a wiring audit, not a claim about runtime behaviour. It reads the
    same source files the firmware builds and checks that each new entry point
    is still present and still carries abort_cancels / routes through the
    shared runtime.
    """
    problems: List[str] = []

    def text(rel: str) -> str:
        path = ROOT / rel
        return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""

    surf = text("components/modal/modal_surf.c")
    for descriptor in ("picker_surface", "progress_surface", "form_ex_surface"):
        marker = "modal_surface_t " + descriptor + " = {"
        start = surf.find(marker)
        if start < 0:
            problems.append("missing descriptor: %s" % descriptor)
            continue
        end = surf.find("};", start)
        block = surf[start:end if end > start else start + 400]
        if ".abort_cancels = true" not in block:
            problems.append("descriptor %s lost abort_cancels" % descriptor)

    commands = text("components/command/modal_commands.c")
    for verb in ("mpick", "mprog", "mformx", "mstack"):
        if ("shell_command_" + verb) not in commands:
            problems.append("missing verb: %s" % verb)
    for call in ("modal_picker_run(", "modal_progress_run(",
                 "modal_form_run_ex(", "modal_session_push(",
                 "modal_surf_set_alpha("):
        if call not in commands:
            problems.append("verb file does not route to %s" % call)

    runtime = text("components/modal/modal.c")
    for call in ("modal_runtime_signal(", "shell_screen_save(",
                 "shell_screen_restore(", "modal_surf_get_alpha(",
                 "modal_session_stack_push("):
        if call not in runtime:
            problems.append("runtime does not call %s" % call)

    pure = text("components/modal/modal_args.c")
    for fn in ("modal_focus_move(", "modal_progress_percent(",
               "modal_session_stack_push(", "modal_session_stack_switch(",
               "modal_alt_tab(", "modal_session_next_index(",
               "modal_session_next_name("):
        if fn not in pure:
            problems.append("pure source does not define %s" % fn)

    # v1.6 app switching: one helper, one input hook, one view, one renderer.
    for call in ("modal_session_cycle(", "modal_set_app_switcher_view(",
                 "modal_alt_tab(", "session_metadata_rotate("):
        if call not in runtime:
            problems.append("modal runtime lost %s" % call)
    frame = text("components/tui/tui_frame.h")
    for fn in ("tui_edge_swipe_bottom_left(", "tui_frame_draw_app_switcher("):
        if fn not in frame:
            problems.append("one tui renderer lost %s" % fn)
    tuic = text("components/tui/tui.c")
    for call in ("tui_set_app_switch_handler(", "tui_frame_draw_app_switcher(",
                 "s_app_switch_handler(1)"):
        if call not in tuic:
            problems.append("tui touch path lost %s" % call)
    if "tui_touch_event_cb(" not in tuic:
        problems.append("tui lost the single touch event callback")
    app = text("main/main.c")
    for call in ("tui_set_app_switch_handler(modal_session_cycle)",
                 "modal_set_app_switcher_view(tui_app_switcher_render)"):
        if call not in app:
            problems.append("main lost the switch registration: %s" % call)
    ui = text("components/command/command_ui.c")
    if "windows switcher" in ui and "tui_app_switcher_render(" not in ui:
        problems.append("windows switcher does not route to the one view")
    return problems


def run_gate(fixture: Path, cc: str, verbose: bool = False,
             source: str | None = None) -> Tuple[int, int]:
    data = load_fixture(fixture)
    problems = audit_wiring()
    if problems:
        raise GateError("wiring audit failed: " + "; ".join(problems))

    with tempfile.TemporaryDirectory(prefix="modal-gate-") as tmp:
        work = Path(tmp)
        harness = work / "modal_harness.c"
        binary = work / ("modal_harness.exe" if os.name == "nt" else "modal_harness")
        harness.write_text(source if source is not None else render_harness(data),
                           encoding="utf-8", newline="\n")
        cmd = [cc, "-std=gnu99", "-Wall", "-I" + str(MODAL_DIR)]
        cmd += ["-I" + str(p) for p in EXTRA_INCLUDES]
        cmd += [str(PURE_SRC), str(harness), "-o", str(binary)]
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(ROOT), timeout=RUN_TIMEOUT)
        if proc.returncode != 0 or not binary.is_file():
            raise GateError("harness compile failed (rc=%d): %s"
                            % (proc.returncode,
                               (proc.stdout + proc.stderr)[-1200:]))
        if verbose:
            print("host_modal_gate: harness checks=%d"
                  % (len(data["focus"]) * 2 + len(data["progress"])
                     + sum(len(s.get("ops", [])) for s in data["sessions"])
                     + len(data["input"]) + len(data["gestures"])
                     + sum(len(s["sessions"]) * 3 + 3
                           for s in data["switch"])
                     + sum(len(c["cells"]) for c in data["switcher"])))
        try:
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 cwd=str(work), timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise GateError("harness run timed out after %ds" % RUN_TIMEOUT)
        if verbose:
            print(run.stdout)
        return parse_output(run.stdout + run.stderr, run.returncode)


def self_test(cc: str) -> int:
    """Red canaries: wrong expectations and forged reports must go red."""
    data = load_fixture(FIXTURE)
    corrupted = json.loads(json.dumps(data))
    corrupted["progress"][0]["pct"] = (int(corrupted["progress"][0]["pct"]) + 1) % 101
    checks: List[Tuple[str, bool]] = []

    # Parser canaries: no compiler, no fixture, no files touched.
    def report(count: int, fail: int = 0, *, names=None):
        names = names or ["c%d" % i for i in range(count)]
        lines = [BEGIN + " tag=selftest"]
        lines += ["modal.check %s %s" % (name, "FAIL" if i < fail else "ok")
                  for i, name in enumerate(names)]
        lines += [SUMMARY + " checks=%d fail=%d" % (count, fail), END]
        return "\n".join(lines) + "\n"

    def must_fail(name: str, raw: str, rc: int = 0) -> None:
        try:
            parse_output(raw, rc)
            ok = False
        except GateError:
            ok = True
        checks.append((name, ok))

    try:
        count, failures = parse_output(report(MIN_CHECKS), 0)
        checks.append(("clean report accepted", count == MIN_CHECKS and
                       failures == 0))
    except GateError as exc:
        print("  clean report unexpectedly failed: %s" % exc)
        checks.append(("clean report accepted", False))
    must_fail("missing begin marker rejected",
              "\n".join(report(MIN_CHECKS).splitlines()[1:]) + "\n")
    must_fail("summary count mismatch rejected",
              report(MIN_CHECKS).replace("checks=%d" % MIN_CHECKS,
                                         "checks=%d" % (MIN_CHECKS + 1)))
    must_fail("failing check rejected", report(MIN_CHECKS, fail=1))
    must_fail("nonzero exit rejected", report(MIN_CHECKS), rc=1)
    must_fail("too few checks rejected", report(MIN_CHECKS - 1))
    must_fail("duplicate check name rejected",
              report(MIN_CHECKS, names=["dup", "dup"] + ["c%d" % i
                                                        for i in range(MIN_CHECKS - 2)]))

    with tempfile.TemporaryDirectory(prefix="modal-gate-self-") as tmp:
        bad = Path(tmp) / "bad.json"
        bad.write_text(json.dumps(corrupted), encoding="utf-8", newline="\n")
        try:
            run_gate(bad, cc)
            caught = False
        except GateError:
            caught = True
        checks.append(("corrupted expectation goes red", caught))

        def must_gate_red(name: str, data_override=None, source=None) -> None:
            path = FIXTURE
            if data_override is not None:
                path = Path(tmp) / (ident(name) + ".json")
                path.write_text(json.dumps(data_override), encoding="utf-8",
                                newline="\n")
            try:
                run_gate(path, cc, source=source)
                red = False
            except GateError:
                red = True
            checks.append((name, red))

        wrong_key = json.loads(json.dumps(data))
        wrong_key["input"][0]["switches"] = not wrong_key["input"][0]["switches"]
        must_gate_red("wrong Alt+Tab expectation goes red", wrong_key)

        wrong_swipe = json.loads(json.dumps(data))
        wrong_swipe["gestures"][0]["swipe"] = not wrong_swipe["gestures"][0]["swipe"]
        must_gate_red("wrong swipe vector goes red", wrong_swipe)

        wrong_order = json.loads(json.dumps(data))
        wrong_order["switch"][0]["expect_order"][-1] = wrong_order["switch"][0]["expect_order"][0]
        must_gate_red("wrong switch order goes red", wrong_order)

        lost = json.loads(json.dumps(data))
        lost["switch"][0]["expect_next"] = "GHOST_SESSION"
        must_gate_red("lost session expectation goes red", lost)

        wrong_cell = json.loads(json.dumps(data))
        wrong_cell["switcher"][0]["cells"][0]["glyph"] = "tr"
        must_gate_red("wrong switcher glyph goes red", wrong_cell)

        # Fake-switch canary: compile the SAME harness with the real switch
        # call replaced by a no-op that returns true. Order/keep checks must
        # fail, so a report that never actually switched cannot go green.
        fake = render_harness(data)
        fake = fake.replace("modal_session_stack_switch(&sw_",
                            "modal_switch_noop(&sw_")
        fake = fake.replace(
            "int main(void)",
            "static bool modal_switch_noop(void *st, const char *name) "
            "{ (void)st; (void)name; return true; }\nint main(void)")
        must_gate_red("fake switch (no-op) goes red", source=fake)

        try:
            count, failures = run_gate(FIXTURE, cc)
            clean = failures == 0 and count >= MIN_CHECKS
        except GateError:
            clean = False
        checks.append(("clean fixture goes green", clean))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    print("host_modal_gate self-test: %s (%d canaries)"
          % ("PASS" if all(ok for _n, ok in checks) else "FAIL", len(checks)))
    return 0 if all(ok for _n, ok in checks) else 1


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Hardware-free modal state gate")
    parser.add_argument("--check", action="store_true", help="run the gate (default)")
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    parser.add_argument("--cc", default=os.environ.get("CC") or
                        shutil.which("gcc") or shutil.which("cc") or "gcc",
                        help="C compiler for the pure-source harness")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test(args.cc)
    try:
        count, failures = run_gate(args.fixture, args.cc, args.verbose)
    except GateError as exc:
        print("HOST MODAL GATE FAIL: %s" % exc)
        return 1
    print("host_modal_gate: PASS (%d checks, %d failures, tag from fixture)"
          % (count, failures))
    print("HOST MODAL GATE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
