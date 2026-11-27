#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_power_gate.py - hardware-free power / sleep-state gate.

The gate compiles the SHIPPED ``components/command/power_commands.c`` and the
SHIPPED pure ``components/header/header_refresh.c`` against deterministic mocks
in ``tools/host/fixtures/power/``. Python only generates the harness and parses
its output: wake eligibility, idle clamping, arm/disarm balance and the
header-refresh minute policy are all decided by the real firmware code.

Mock state comes from ``tools/host/fixtures/power/state.json`` (per-board GPIO
lines, idle clamp cases, sleep/deepsleep cases, clock-minute cases). A pass is a
BOARD-FREE PRE-GATE ONLY; it is never hardware power sign-off (harness.md §0).

Usage:
    python tools/host_power_gate.py              # --check (default)
    python tools/host_power_gate.py --check
    python tools/host_power_gate.py --self-test  # red canaries
    python tools/host_power_gate.py --fixture PATH --cc gcc
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
from typing import Any, Dict, List, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tools" / "host" / "fixtures" / "power" / "state.json"
FIX_DIR = FIXTURE.parent
INCLUDE = FIX_DIR / "include"
MOCK_SRC = FIX_DIR / "mock_state.c"
POWER_SRC = ROOT / "components" / "command" / "power_commands.c"
PURE_SRC = ROOT / "components" / "header" / "header_refresh.c"

BEGIN = "power.gate.begin"
END = "power.gate.end"
SUMMARY = "power.gate.summary"
CHECK_RE = re.compile(r"^power\.check\s+([A-Za-z0-9_.]+)\s+(ok|FAIL)$")
SUMMARY_RE = re.compile(r"^power\.gate\.summary\s+checks=(\d+)\s+fail=(\d+)$")


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


def ident(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", str(value))


def load_fixture(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        raise GateError("fixture is missing: %s" % path)
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError("fixture unreadable: %s" % exc)
    if not isinstance(data.get("tag"), str) or not data["tag"]:
        raise GateError("fixture has no tag")
    for group, minimum in (("idle_clamp", 4), ("idle_interval", 5), ("boards", 2),
                           ("sleep_cases", 2), ("deepsleep_cases", 2),
                           ("clock_minute", 2)):
        if not isinstance(data.get(group), list) or len(data[group]) < minimum:
            raise GateError("fixture group %r is missing or vacuous (%d < %d)"
                            % (group, len(data.get(group, [])), minimum))
    for board in data["boards"]:
        for key in ("name", "wake_gpio", "wake_level", "touch_int_gpio",
                    "keyboard_int_gpio", "deep_mask", "expect"):
            if key not in board:
                raise GateError("board case lacks %r" % key)
        for key in ("user_armed", "touch_armed", "keyboard_armed", "deep_gpio"):
            if key not in board["expect"]:
                raise GateError("board %s lacks expect.%s" % (board["name"], key))
    return data


def _check(name: str, condition: str) -> str:
    return '    CHECK("%s", %s);' % (name, condition)


def _cstr(value: str) -> str:
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _bool(value: Any) -> str:
    return "true" if value else "false"


def _int(value: Any) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(int(value))


def render_board(data: Dict[str, Any], board: Dict[str, Any]) -> List[str]:
    """Generate the C checks for one board profile."""
    tag = ident(board["name"])
    exp = board["expect"]
    out: List[str] = []
    out.append("    /* ===== board profile: %s ===== */" % board["name"])
    out.append("    mock_reset();")

    # Pure idle clamp, plus the integrated setter/getter (state under the lock).
    for case in data["idle_clamp"]:
        out.append(_check("%s.clamp.%s" % (tag, ident(case["name"])),
                          "shell_power_clamp_idle_seconds(%d) == %d"
                          % (int(case["seconds"]), int(case["expected"]))))
    out.extend([
        "    shell_power_set_idle_timeout(-7);",
        _check(tag + ".set_negative", "shell_power_get_idle_timeout() == 0"),
        "    shell_power_set_idle_timeout(86401);",
        _check(tag + ".set_huge",
               "shell_power_get_idle_timeout() == P4_CONFIG_POWER_IDLE_DISPLAY_MAX_SECS"),
        "    shell_power_set_idle_timeout(60);",
        _check(tag + ".set_normal", "shell_power_get_idle_timeout() == 60"),
    ])

    # Pure idle interval, plus the production accessor with mock time.
    for case in data["idle_interval"]:
        out.append(_check("%s.interval.%s" % (tag, ident(case["name"])),
                          "shell_power_idle_remaining_ms(%d, %dLL, %dLL, %s) == %d"
                          % (int(case["timeout"]), int(case["now_us"]),
                             int(case["last_us"]), _bool(case["off"]),
                             int(case["expected"]))))
    out.extend([
        "    mock_reset();",
        "    g_power_mock.now_us = 1000000;",
        "    shell_power_set_idle_timeout(60);",
        "    shell_power_notify_activity();",
        "    g_power_mock.now_us = 31000000;",
        _check(tag + ".accessor_half", "shell_power_ms_until_idle_off() == 30000"),
    ])

    # Idle display-off and activity wake (no new task, LVGL indev mock only).
    out.extend([
        "    mock_reset();",
        "    shell_power_set_idle_timeout(1);",
        "    g_power_mock.now_us = 1000000;",
        "    shell_power_notify_activity();",
        "    g_power_mock.now_us = 3000000;",
        "    clear_transcript();",
        "    shell_power_idle_tick();",
        _check(tag + ".idle_off_state",
               "display_get_power_state() == DISPLAY_POWER_SLEEP"),
        _check(tag + ".idle_off_notice", 'has("display off (idle)")'),
        _check(tag + ".idle_off_no_deadline", "shell_power_ms_until_idle_off() == 0"),
        "    g_power_mock.indev_present = true;",
        "    g_power_mock.indev_pressed = true;",
        "    clear_transcript();",
        "    shell_power_idle_tick();",
        _check(tag + ".idle_touch_wake",
               "display_get_power_state() == DISPLAY_POWER_ON && has(\"power: display on\")"),
        "    g_power_mock.indev_pressed = false;",
    ])

    # Wake-source eligibility is checked through the shipped pure predicate.
    for label, gpio_key in (("user", "wake_gpio"), ("touch", "touch_int_gpio"),
                            ("keyboard", "keyboard_int_gpio")):
        gpio = int(board[gpio_key])
        if gpio < 0:
            continue
        expect = _bool(exp["deep_gpio"] if label == "user" else False)
        out.append(_check("%s.deep_eligible.%s" % (tag, label),
                          "shell_power_deep_wake_gpio_eligible(%d) == %s" % (gpio, expect)))

    # `battery sleep on/off` must really change PM state and report it honestly.
    out.extend([
        "    mock_reset();",
        "    clear_transcript();",
        "    char *battery_on[] = {\"battery\", \"sleep\", \"on\"};",
        "    shell_command_battery(3, battery_on);",
        _check(tag + ".battery_sleep_on",
               "shell_power_light_sleep_requested() && g_power_mock.pm_light_sleep_enable == 1 "
               '&& has("light sleep")'),
        "    char *battery_off[] = {\"battery\", \"sleep\", \"off\"};",
        "    shell_command_battery(3, battery_off);",
        _check(tag + ".battery_sleep_off",
               "!shell_power_light_sleep_requested() && g_power_mock.pm_light_sleep_enable == 0"),
    ])

    # Light sleep: timer arming, per-board pin arming, honesty and no leakage.
    for case in data["sleep_cases"]:
        stem = "%s.sleep.%s" % (tag, ident(case["name"]))
        argv_name = ident(stem)
        secs = int(case["seconds"])
        want_timer = bool(case["expect_timer"])
        out.extend([
            "    mock_reset();",
            "    apply_board_pins();",
            "    clear_transcript();",
            "    char *%s[] = {\"sleep\", \"%d\"};" % (argv_name, secs),
            "    shell_command_sleep(2, %s);" % argv_name,
            _check(stem + ".entered", "g_power_mock.light_sleep_calls == 1"),
        ])
        if want_timer:
            out.append(_check(stem + ".timer", "g_power_mock.timer_armed == false && "
                              "g_power_mock.timer_us == %dULL" % (secs * 1000000)))
        else:
            out.append(_check(stem + ".no_timer", "g_power_mock.timer_disable_calls >= 2 && "
                              "has(\"no timer set\")"))
        out.extend([
            _check(stem + ".no_gpio_leak", "g_power_mock.gpio_wake_source_enabled == false"),
            _check(stem + ".no_timer_leak", "g_power_mock.timer_armed == false"),
            _check(stem + ".pins_balanced",
                   "g_power_mock.gpio_enable_calls == g_power_mock.gpio_disable_calls"),
            _check(stem + ".woke_notice", 'has("woke up (cause=")'),
        ])
        if exp["user_armed"]:
            out.append(_check(stem + ".user_pin",
                              "g_power_mock.gpio_at_sleep[%d]" % int(board["wake_gpio"])))
        if exp["touch_armed"]:
            out.append(_check(stem + ".touch_pin",
                              "g_power_mock.gpio_at_sleep[%d]" % int(board["touch_int_gpio"])))
        if exp["keyboard_armed"]:
            out.append(_check(stem + ".keyboard_pin",
                              "g_power_mock.gpio_at_sleep[%d]" % int(board["keyboard_int_gpio"])))
        out.extend([
            _check(stem + ".user_msg",
                   'has("GPIO wake on GPIO%d") == %s'
                   % (int(board["wake_gpio"]), _bool(exp["user_armed"]))),
            _check(stem + ".touch_msg",
                   'has("touch wake armed") == %s' % _bool(exp["touch_armed"])),
            _check(stem + ".keyboard_msg",
                   'has("keyboard wake armed") == %s' % _bool(exp["keyboard_armed"])),
            _check(stem + ".no_source_msg",
                   'has("no GPIO wake source") == %s'
                   % _bool(not any(exp[k] for k in
                                   ("user_armed", "touch_armed", "keyboard_armed")))),
        ])

    # Failed sleep and failed pin arming must be reported, not silently faked.
    out.extend([
        "    mock_reset();",
        "    apply_board_pins();",
        "    g_power_mock.light_sleep_result = ESP_FAIL;",
        "    clear_transcript();",
        "    char *fail_argv[] = {\"sleep\", \"1\"};",
        "    shell_command_sleep(2, fail_argv);",
        _check(tag + ".sleep_failure_honest", 'has("light sleep failed")'),
        _check(tag + ".sleep_failure_disarmed",
               "g_power_mock.gpio_wake_source_enabled == false && "
               "g_power_mock.gpio_enable_calls == g_power_mock.gpio_disable_calls"),
        "    mock_reset();",
        "    apply_board_pins();",
        "    g_power_mock.gpio_enable_fail = true;",
        "    clear_transcript();",
        "    shell_command_sleep(2, fail_argv);",
    ])
    if any(exp[k] for k in ("user_armed", "touch_armed", "keyboard_armed")):
        out.append(_check(tag + ".arm_failure_honest", 'has("failed to enable")'))
    else:
        out.append(_check(tag + ".arm_failure_honest", 'has("no GPIO wake source")'))

    # Deep sleep: timer, RTC GPIO eligibility, honest messages, stale timer.
    for case in data["deepsleep_cases"]:
        stem = "%s.deep.%s" % (tag, ident(case["name"]))
        argv_name = ident(stem)
        secs = int(case["seconds"])
        want_timer = bool(case["expect_timer"])
        out.extend([
            "    mock_reset();",
            "    apply_board_pins();",
            "    clear_transcript();",
            "    char *%s[] = {\"deepsleep\", \"%d\"};" % (argv_name, secs),
            "    shell_command_deepsleep(2, %s);" % argv_name,
            _check(stem + ".entered", "g_power_mock.deep_sleep_calls == 1"),
            _check(stem + ".timer", "g_power_mock.timer_armed == %s" % _bool(want_timer)),
            _check(stem + ".deep_gpio",
                   "g_power_mock.deep_gpio_armed == %s" % _bool(exp["deep_gpio"])),
            _check(stem + ".deep_msg",
                   'has("deepsleep: GPIO wake on") == %s' % _bool(exp["deep_gpio"])),
            _check(stem + ".no_timer_msg", 'has("no timer set") == %s' % _bool(not want_timer)),
            _check(stem + ".no_source_msg",
                   'has("no wake source armed") == %s'
                   % _bool(not want_timer and not exp["deep_gpio"])),
        ])
        if want_timer:
            out.append(_check(stem + ".timer_us",
                              "g_power_mock.timer_us == %dULL" % (secs * 1000000)))
    out.extend([
        "    /* Stale wake flag: an armed timer must not survive a later `deepsleep 0`. */",
        "    mock_reset();",
        "    apply_board_pins();",
        "    char *stale_first[] = {\"deepsleep\", \"30\"};",
        "    shell_command_deepsleep(2, stale_first);",
        "    char *stale_second[] = {\"deepsleep\", \"0\"};",
        "    shell_command_deepsleep(2, stale_second);",
        _check(tag + ".stale_timer_cleared", "g_power_mock.timer_armed == false"),
    ])

    # Clock minute must never be skipped, including while the display is off.
    for case in data["clock_minute"]:
        stem = "%s.clock.%s" % (tag, ident(case["name"]))
        secs = int(case["seconds_to_next_minute"])
        bound = int(case["expected_max"])
        out.extend([
            "    {",
            "        header_refresh_state_t st = {0};",
            "        uint32_t off_ms;",
            "        st.display_off = true;",
            "        st.clock_enabled = true;",
            "        st.seconds_to_next_minute = %d;" % secs,
            "        off_ms = header_refresh_interval_ms(&st);",
            _check(stem + ".off_bounded",
                   "off_ms >= P4_CONFIG_HEADER_REFRESH_MIN_MS && off_ms <= %d" % bound),
            _check(stem + ".off_wake_cadence",
                   "off_ms == P4_CONFIG_HEADER_REFRESH_WAKE_MS"),
            "        st.display_off = false;",
            _check(stem + ".on_no_skip",
                   "header_refresh_interval_ms(&st) <= %d" % bound),
            "        st.seconds_to_next_minute = 60;",
            "        st.ms_to_idle_off = 500;",
            _check(stem + ".idle_deadline",
                   "header_refresh_interval_ms(&st) == 500"),
            "    }",
        ])
    return out


def render_harness(data: Dict[str, Any], board: Dict[str, Any]) -> str:
    body = render_board(data, board)
    return "\n".join([
        "/* GENERATED by tools/host_power_gate.py - do not edit. */",
        '#include "mock_state.h"',
        '#include "header_refresh.h"',
        '#include "p4minishell_config.h"',
        "#include <stdio.h>",
        "#include <string.h>",
        "",
        "static int s_checks = 0;",
        "static int s_failures = 0;",
        "#define CHECK(name, cond) do { s_checks++; if (cond) {"
        ' printf("power.check %s ok\\n", (name)); } else { s_failures++;'
        ' printf("power.check %s FAIL\\n", (name)); } } while (0)',
        "",
        "static bool has(const char *needle)",
        "{",
        "    return strstr(g_power_mock.transcript, needle) != NULL;",
        "}",
        "",
        "static void clear_transcript(void)",
        "{",
        "    g_power_mock.transcript[0] = '\\0';",
        "}",
        "",
        "static void apply_board_pins(void)",
        "{",
        "    g_power_mock.touch_int_gpio = %d;" % int(board["touch_int_gpio"]),
        "    g_power_mock.keyboard_int_gpio = %d;" % int(board["keyboard_int_gpio"]),
        "}",
        "",
        "int main(void)",
        "{",
        '    printf("' + BEGIN + ' tag=' + data["tag"] + ' board=' + ident(board["name"]) + '\\n");',
    ] + body + [
        '    printf("' + SUMMARY + ' checks=%d fail=%d\\n", s_checks, s_failures);',
        '    printf("' + END + '\\n");',
        "    return s_failures == 0 ? 0 : 1;",
        "}",
        "",
    ])


def parse_output(raw: str, returncode: int) -> Tuple[int, int]:
    lines = [line.strip() for line in raw.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    begins = [l for l in lines if l.startswith(BEGIN + " ")]
    ends = [l for l in lines if l == END]
    summaries = [SUMMARY_RE.match(l) for l in lines if l.startswith(SUMMARY + " ")]
    summaries = [m for m in summaries if m]
    checks = [CHECK_RE.match(l) for l in lines if l.startswith("power.check ")]
    checks = [m for m in checks if m]

    if len(begins) != 1:
        raise GateError("expected exactly one begin marker, got %d" % len(begins))
    if len(ends) != 1:
        raise GateError("expected exactly one end marker, got %d" % len(ends))
    if len(summaries) != 1:
        raise GateError("expected exactly one summary marker, got %d" % len(summaries))
    names = [m.group(1) for m in checks]
    if len(names) != len(set(names)):
        raise GateError("duplicate power.check name in harness output")
    count = int(summaries[0].group(1))
    fails = int(summaries[0].group(2))
    if count != len(checks):
        raise GateError("summary checks=%d but %d check lines" % (count, len(checks)))
    if fails != sum(1 for m in checks if m.group(2) == "FAIL"):
        raise GateError("summary fail=%d disagrees with FAIL lines" % fails)
    if returncode != 0 or fails:
        bad = [m.group(1) for m in checks if m.group(2) == "FAIL"]
        raise GateError("harness red: rc=%d, failing checks: %s"
                        % (returncode, ", ".join(bad[:8]) or "(summary only)"))
    return count, fails


def audit_wiring() -> List[str]:
    """Reject a tree where a power verb or its single wake path went missing."""
    problems: List[str] = []

    def text(rel: str) -> str:
        path = ROOT / rel
        return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""

    power = text("components/command/power_commands.c")
    for marker in (
        "void shell_command_power(", "void shell_command_sleep(",
        "void shell_command_deepsleep(", "void shell_command_shutdown(",
        "void shell_command_battery(", "void shell_power_idle_tick(",
        "shell_power_set_idle_timeout(", "shell_power_get_idle_timeout(",
        "shell_power_notify_activity(", "shell_power_light_sleep_requested(",
        "shell_power_clamp_idle_seconds(",
        "shell_power_idle_remaining_ms(", "shell_power_deep_wake_gpio_eligible(",
        "display_get_touch_int_gpio()", "esp_light_sleep_start()",
        "esp_deep_sleep_enable_gpio_wakeup(", "esp_sleep_enable_gpio_wakeup()",
        "esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO)",
        "esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER)",
    ):
        if marker not in power:
            problems.append("power_commands.c lost %s" % marker)
    for forbidden in ("xTaskCreate", "esp_timer_create", "lv_timer_create"):
        if forbidden in power:
            problems.append("power_commands.c added a new task/timer: %s" % forbidden)

    shell_core = text("components/shell/shell_core.c")
    for marker in ('{ "power",', '{ "sleep",', '{ "deepsleep",', "CONFIG.SYS DISPLAY_TIMEOUT="):
        if marker not in shell_core:
            problems.append("shell_core.c help lost %s" % marker)

    main = text("main/main.c")
    for marker in ("shell_power_idle_tick();", "shell_header_refresh_interval_ms()"):
        if marker not in main:
            problems.append("main.c lost %s" % marker)

    pure = text("components/header/header_refresh.c")
    for marker in ("display_off", "seconds_to_next_minute", "ms_to_idle_off"):
        if marker not in pure:
            problems.append("header_refresh.c lost %s" % marker)

    config_cmd = text("components/command/config_cmd.c")
    if "shell_power_set_idle_timeout" not in config_cmd or "DISPLAY_TIMEOUT" not in config_cmd:
        problems.append("CONFIG.SYS DISPLAY_TIMEOUT is no longer wired to power idle")
    boot = text("components/boot/boot.c")
    if "power idle" not in boot or "DISPLAY_TIMEOUT" not in boot:
        problems.append("boot CONFIG.SYS DISPLAY_TIMEOUT directive is no longer wired")
    return problems


def compile_and_run(data: Dict[str, Any], board: Dict[str, Any], cc: str,
                    work: Path, source: Path, verbose: bool) -> Tuple[int, int]:
    harness = work / ("power_harness_%s.c" % ident(board["name"]))
    binary = work / ("power_harness_%s%s" % (ident(board["name"]),
                                             ".exe" if os.name == "nt" else ""))
    harness.write_text(render_harness(data, board), encoding="utf-8", newline="\n")
    defines = [
        "-DP4_CONFIG_POWER_WAKE_GPIO=%d" % int(board["wake_gpio"]),
        "-DBOARD_CFG_TAB5KBD_INT_GPIO=%d" % int(board["keyboard_int_gpio"]),
        "-DSOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK=%s" % board["deep_mask"],
    ]
    cmd = ([cc, "-std=gnu99", "-Wall", "-I", str(FIX_DIR), "-I", str(INCLUDE),
            "-I", str(INCLUDE / "esp_adc"), "-I", str(ROOT / "components" / "header"),
            str(source), str(MOCK_SRC), str(PURE_SRC), str(harness)] + defines +
           ["-o", str(binary)])
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT), timeout=120)
    if proc.returncode != 0 or not binary.is_file():
        raise GateError("harness compile failed for %s (rc=%d): %s"
                        % (board["name"], proc.returncode,
                           (proc.stdout + proc.stderr)[-1600:]))
    try:
        run = subprocess.run([str(binary)], capture_output=True, text=True,
                             cwd=str(work), timeout=60)
    except subprocess.TimeoutExpired:
        raise GateError("harness run timed out for %s" % board["name"])
    if verbose:
        print(run.stdout)
    count, _fails = parse_output(run.stdout + run.stderr, run.returncode)
    minimum = int(data.get("min_checks", 0))
    if count < minimum:
        raise GateError("vacuous harness for %s: %d checks, need >= %d"
                        % (board["name"], count, minimum))
    return count, 0


def run_gate(fixture: Path, cc: str, source: Path | None = None,
             verbose: bool = False) -> Tuple[int, int]:
    data = load_fixture(fixture)
    problems = audit_wiring()
    if problems:
        raise GateError("wiring audit failed: " + "; ".join(problems))
    src = source if source is not None else POWER_SRC
    if not src.is_file():
        raise GateError("power source is missing: %s" % src)
    with tempfile.TemporaryDirectory(prefix="power-gate-") as tmp:
        work = Path(tmp)
        # Compile a byte-for-byte copy from a neutral directory so quoted
        # includes resolve to our mocks, never to the real component headers.
        copied = work / "power_commands.c"
        copied.write_bytes(src.read_bytes())
        total = 0
        for board in data["boards"]:
            count, _ = compile_and_run(data, board, cc, work, copied, verbose)
            total += count
        return total, 0


def self_test(cc: str) -> int:
    """Red canaries: forged reports, a stale wake flag and a wrong idle clamp."""
    data = load_fixture(FIXTURE)
    checks: List[Tuple[str, bool]] = []

    def report(count: int, fail: int = 0, *, names=None):
        names = names or ["c%d" % i for i in range(count)]
        lines = [BEGIN + " tag=selftest"]
        lines += ["power.check %s %s" % (name, "FAIL" if i < fail else "ok")
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
        count, failures = parse_output(report(2), 0)
        checks.append(("clean report accepted", count == 2 and failures == 0))
    except GateError as exc:
        print("  clean report unexpectedly failed: %s" % exc)
        checks.append(("clean report accepted", False))
    must_fail("missing begin marker rejected",
              "\n".join(report(2).splitlines()[1:]) + "\n")
    must_fail("summary count mismatch rejected", report(2).replace("checks=2", "checks=3"))
    must_fail("failing check rejected", report(2, fail=1))
    must_fail("nonzero exit rejected", report(2), rc=1)
    must_fail("duplicate check name rejected",
              report(2, names=["dup", "dup"]))

    with tempfile.TemporaryDirectory(prefix="power-gate-self-") as tmp:
        work = Path(tmp)
        source = work / "power_commands.c"

        # Canary 1: stale wake flag - the global GPIO wake source is not cleared.
        stale = POWER_SRC.read_text(encoding="utf-8")
        if "esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);" not in stale:
            raise GateError("cannot stage stale-wake canary: cleanup line missing")
        stale = stale.replace("esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);",
                              "/* canary: stale wake flag */", 1)
        source.write_text(stale, encoding="utf-8", newline="\n")
        try:
            run_gate(FIXTURE, cc, source=source)
            caught = False
        except GateError:
            caught = True
        checks.append(("stale wake flag goes red", caught))

        # Canary 2: wrong idle interval - clamp window widened by one second.
        wrong = POWER_SRC.read_text(encoding="utf-8")
        marker = "if (seconds > (int64_t)SHELL_POWER_IDLE_DISPLAY_MAX_SECS) {"
        if marker not in wrong:
            raise GateError("cannot stage idle-clamp canary: clamp line missing")
        wrong = wrong.replace(marker,
                              "if (seconds > (int64_t)SHELL_POWER_IDLE_DISPLAY_MAX_SECS + 1) {",
                              1)
        source.write_text(wrong, encoding="utf-8", newline="\n")
        try:
            run_gate(FIXTURE, cc, source=source)
            caught = False
        except GateError:
            caught = True
        checks.append(("wrong idle interval goes red", caught))

        # Canary 3: a corrupted fixture expectation must not pass either.
        bad = json.loads(json.dumps(data))
        bad["idle_clamp"][0]["expected"] = 99
        bad_fixture = work / "bad.json"
        bad_fixture.write_text(json.dumps(bad), encoding="utf-8", newline="\n")
        try:
            run_gate(bad_fixture, cc)
            caught = False
        except GateError:
            caught = True
        checks.append(("corrupted expectation goes red", caught))

        try:
            count, failures = run_gate(FIXTURE, cc)
            clean = failures == 0 and count >= int(data.get("min_checks", 0))
        except GateError as exc:
            print("  clean fixture unexpectedly failed: %s" % exc)
            clean = False
        checks.append(("clean fixture goes green", clean))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    print("host_power_gate self-test: %s (%d canaries)"
          % ("PASS" if all(ok for _n, ok in checks) else "FAIL", len(checks)))
    return 0 if all(ok for _n, ok in checks) else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Hardware-free power/sleep-state gate")
    parser.add_argument("--check", action="store_true", help="run the gate (default)")
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    parser.add_argument("--cc", default=os.environ.get("CC") or
                        shutil.which("gcc") or shutil.which("cc") or "gcc",
                        help="C compiler for the mock harness")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test(args.cc)
    try:
        count, failures = run_gate(args.fixture, args.cc, verbose=args.verbose)
    except GateError as exc:
        print("HOST POWER GATE FAIL: %s" % exc)
        return 1
    print("host_power_gate: PASS (%d checks, %d failures, %d board profiles)"
          % (count, failures, len(load_fixture(args.fixture)["boards"])))
    print("HOST POWER GATE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
