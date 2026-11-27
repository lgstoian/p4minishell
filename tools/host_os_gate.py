#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_os_gate.py - hardware-free gate for the read-only `os` facade.

The gate compiles the SAME pure parse/format code the firmware ships: the
OS_PURE_DECL block from ``components/command/command.h`` and the OS_PURE_IMPL
block from ``components/command/os_commands.c``. A generated C harness feeds
mock accessor snapshots from ``tools/host/fixtures/os/snapshot.json`` into that
code and prints exact ``key=value`` lines plus structured check results.
Python re-implements no formatter and opens no serial port.

Baselines in the fixture are exact: a stale expected value makes the harness
red. A field whose accessor is marked unavailable must render ``unavailable``,
never a fabricated value. The parser rejects vacuous or inconsistent harness
output, and ``--self-test`` red-canaries all three failure modes (missing
accessor, stale value, false green) plus the source-wiring audit.

This is a board-free pre-gate only. It says nothing about on-device behaviour;
``harness.md`` §0/§2.2 still governs hardware sign-off.

Usage:
    python tools/host_os_gate.py              # --check (default)
    python tools/host_os_gate.py --check
    python tools/host_os_gate.py --fixture PATH
    python tools/host_os_gate.py --cc gcc
    python tools/host_os_gate.py --self-test  # red canaries, no board
"""
from __future__ import annotations

import argparse
import copy
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
FIXTURE = ROOT / "tools" / "host" / "fixtures" / "os" / "snapshot.json"
IMPL = ROOT / "components" / "command" / "os_commands.c"
HEADER = ROOT / "components" / "command" / "command.h"
DISPATCH = ROOT / "components" / "command" / "command.c"
HELP = ROOT / "components" / "shell" / "shell_core.c"
TEST_SRC = ROOT / "test" / "main" / "test_os_facade.c"
TEST_MAIN = ROOT / "test" / "main" / "test_main.c"
TEST_CMAKE = ROOT / "test" / "main" / "CMakeLists.txt"

BEGIN = "os.gate.begin"
END = "os.gate.end"
SUMMARY = "os.gate.summary"
CHECK_RE = re.compile(r"^os\.check\s+([A-Za-z0-9_.]+)\s+(ok|FAIL)$")
SUMMARY_RE = re.compile(r"^os\.gate\.summary\s+checks=(\d+)\s+fail=(\d+)$")

DECL_BEGIN = "/* OS_PURE_DECL_BEGIN */"
DECL_END = "/* OS_PURE_DECL_END */"
IMPL_BEGIN = "/* OS_PURE_IMPL_BEGIN */"
IMPL_END = "/* OS_PURE_IMPL_END */"

PURE_FNS = (
    "command_os_parse",
    "command_os_mode_allows",
    "command_os_area_name",
    "command_os_status_name",
    "command_os_format_field",
    "command_os_format_status",
)
AREA_ENUM = {
    "pm": "OS_AREA_PM", "drv": "OS_AREA_DRV", "io": "OS_AREA_IO",
    "fs": "OS_AREA_FS", "net": "OS_AREA_NET", "mem": "OS_AREA_MEM",
    "disk": "OS_AREA_DISK", "sec": "OS_AREA_SEC", "pwr": "OS_AREA_PWR",
    "ver": "OS_AREA_VER", "invalid": "OS_AREA_INVALID",
}
MODE_ENUM = {
    "status": "OS_MODE_STATUS", "list": "OS_MODE_LIST",
    "free": "OS_MODE_FREE", "invalid": "OS_MODE_INVALID",
}
MIN_CHECKS = 40
MIN_CASES = 8
RUN_TIMEOUT = 60


class GateError(RuntimeError):
    """Any condition that must make the gate red."""


def cstr(value: str) -> str:
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_fixture(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        raise GateError("fixture is missing: %s" % path)
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError("fixture unreadable: %s" % exc)
    if not isinstance(data.get("tag"), str) or not data["tag"]:
        raise GateError("fixture has no tag")
    cases = data.get("cases")
    if not isinstance(cases, list) or len(cases) < MIN_CASES:
        raise GateError("fixture has too few cases (%d < %d)"
                        % (len(cases) if isinstance(cases, list) else 0, MIN_CASES))
    seen = set()
    for case in cases:
        name = case.get("name")
        if not isinstance(name, str) or not name:
            raise GateError("case has no name")
        if name in seen:
            raise GateError("duplicate case name: %s" % name)
        seen.add(name)
        if case.get("area") not in AREA_ENUM or case.get("mode") not in MODE_ENUM:
            raise GateError("case %s has an unknown area/mode" % name)
        argv = case.get("argv")
        if not isinstance(argv, list) or not argv or len(argv) > 7:
            raise GateError("case %s has a bad argv" % name)
        for field in case.get("fields", []):
            for key in ("key", "expected"):
                if not isinstance(field.get(key), str) or not field[key]:
                    raise GateError("case %s field lacks %s" % (name, key))
    return data


def read_sources() -> Dict[str, str]:
    paths = {
        "command.c": DISPATCH,
        "command.h": HEADER,
        "os_commands.c": IMPL,
        "shell_core.c": HELP,
        "test_os_facade.c": TEST_SRC,
        "test_main.c": TEST_MAIN,
        "CMakeLists.txt": TEST_CMAKE,
    }
    out = {}
    for key, path in paths.items():
        if not path.is_file():
            raise GateError("source is missing: %s" % path)
        out[key] = path.read_text(encoding="utf-8", errors="replace")
    return out


def extract_block(text: str, begin: str, end: str) -> str:
    start = text.find(begin)
    if start < 0:
        raise GateError("missing block marker: %s" % begin)
    start += len(begin)
    stop = text.find(end, start)
    if stop < 0:
        raise GateError("missing block marker: %s" % end)
    return text[start:stop]


def audit_wiring(src: Dict[str, str]) -> List[str]:
    """Static wiring audit: reject a tree where the facade silently vanished."""
    problems: List[str] = []
    dispatch = src["command.c"]
    if 'shell_text_equals_ignore_case(argv[0], "os")' not in dispatch:
        problems.append("dispatcher lost the `os` arm")
    if "batch_set_errorlevel(shell_command_os(argc, argv));" not in dispatch:
        problems.append("`os` arm does not set ERRORLEVEL from the facade")
    header = src["command.h"]
    if "int shell_command_os(int argc, char **argv);" not in header:
        problems.append("command.h lost the shell_command_os prototype")
    if '{ "os",' not in src["shell_core.c"]:
        problems.append("help table lost the `os` line")
    if "pwr [status" not in src["shell_core.c"]:
        problems.append("help line lost the `pwr` area")
    if "os_pwr(" not in src["os_commands.c"]:
        problems.append("os_commands.c lost the pwr adapter")
    if "test_os_facade.c" not in src["CMakeLists.txt"]:
        problems.append("test CMakeLists lost test_os_facade.c")
    names = re.findall(r"void\s+(test_os_\w+)\s*\(void\)", src["test_os_facade.c"])
    if len(names) < 10:
        problems.append("test_os_facade.c has too few declared cases")
    for name in names:
        if "RUN_TEST(%s)" % name not in src["test_main.c"]:
            problems.append("test_main.c never runs %s" % name)
    try:
        decl = extract_block(header, DECL_BEGIN, DECL_END)
        impl = extract_block(src["os_commands.c"], IMPL_BEGIN, IMPL_END)
    except GateError as exc:
        problems.append(str(exc))
        return problems
    for fn in PURE_FNS:
        if fn + "(" not in decl:
            problems.append("pure declarations lost %s" % fn)
        if fn + "(" not in impl:
            problems.append("pure implementation lost %s" % fn)
    if "OS_AREA_PWR" not in decl or "OS_AREA_PWR" not in impl:
        problems.append("pure region lost the pwr area")
    if "shell_execute_command_core(" in impl or "int shell_command_ps(" in impl:
        problems.append("pure region must not own a second dispatcher or task walker")
    if '"unavailable"' not in impl:
        problems.append("pure formatter lost its explicit unavailable verdict")
    return problems


def _check(name: str, condition: str) -> str:
    return '    CHECK(%s, %s);' % (cstr(name), condition)


def render_case(idx: int, case: Dict[str, Any]) -> List[str]:
    prefix = "case.%d" % idx
    area = AREA_ENUM[case["area"]]
    mode = MODE_ENUM[case["mode"]]
    body: List[str] = [
        "    {",
        "        char *argv[8] = {%s};" %
        ", ".join(cstr(t) for t in case["argv"]),
        "        os_area_t area = OS_AREA_INVALID;",
        "        os_mode_t mode = OS_MODE_INVALID;",
        "        bool bare = false;",
        "        char line[192];",
        "        int rc = command_os_parse(%d, argv, &area, &mode, &bare);"
        % len(case["argv"]),
        _check(prefix + ".parse.rc", "rc == %d" % int(case["rc"])),
        _check(prefix + ".parse.area", "area == %s" % area),
        _check(prefix + ".parse.mode", "mode == %s" % mode),
        _check(prefix + ".parse.bare", "bare == %s" %
               ("true" if case["bare"] else "false")),
    ]
    expected_allowed = case["rc"] == 0 and case.get("mode_allowed", True)
    if case["rc"] == 0:
        body.append(_check(prefix + ".area_name",
                           "strcmp(command_os_area_name(%s), %s) == 0" %
                           (area, cstr(case["area"]))))
    body.append(_check(prefix + ".mode_allows",
                       "command_os_mode_allows(%s, %s) == %s" %
                       (area, mode, "true" if expected_allowed else "false")))
    for fidx, field in enumerate(case["fields"]):
        value = "NULL" if field.get("value") is None else cstr(field["value"])
        avail = "true" if field.get("available") else "false"
        body.extend([
            "        command_os_format_field(%s, %s, %s, line, sizeof(line));" %
            (cstr(field["key"]), value, avail),
            _check("%s.field.%d" % (prefix, fidx),
                   "strcmp(line, %s) == 0" % cstr(field["expected"])),
        ])
    if case["rc"] == 0:
        body.extend([
            "        command_os_format_status(%s, %s, line, sizeof(line));" %
            (area, "true" if case["status"] == "ok" else "false"),
            _check(prefix + ".status",
                   "strcmp(line, %s) == 0" %
                   cstr("os.%s.status=%s" % (case["area"], case["status"]))),
        ])
    body.append("    }")
    return body


def render_harness(data: Dict[str, Any], decl: str, impl: str) -> str:
    body: List[str] = []
    for idx, case in enumerate(data["cases"]):
        body.extend(render_case(idx, case))
    return "\n".join([
        "/* GENERATED by tools/host_os_gate.py - do not edit. */",
        "#include <stdbool.h>",
        "#include <stddef.h>",
        "#include <stdio.h>",
        "#include <string.h>",
        "",
        decl,
        impl,
        "",
        "static int s_checks = 0;",
        "static int s_failures = 0;",
        '#define CHECK(name, cond) do { s_checks++; if (cond) {'
        ' printf("os.check %s ok\\n", (name)); } else { s_failures++;'
        ' printf("os.check %s FAIL\\n", (name)); } } while (0)',
        "",
        "int main(void)",
        "{",
        '    printf("' + BEGIN + ' tag=' + data["tag"] + '\\n");',
    ] + body + [
        '    printf("' + SUMMARY + ' checks=%d fail=%d\\n", s_checks, s_failures);',
        '    printf("' + END + '\\n");',
        "    return s_failures == 0 ? 0 : 1;",
        "}",
        "",
    ])


def compile_and_run(harness: str, cc: str) -> Tuple[str, int]:
    with tempfile.TemporaryDirectory(prefix="os-gate-") as tmp:
        work = Path(tmp)
        src = work / "os_harness.c"
        binary = work / ("os_harness.exe" if os.name == "nt" else "os_harness")
        src.write_text(harness, encoding="utf-8", newline="\n")
        cmd = [cc, "-std=gnu99", "-Wall", str(src), "-o", str(binary)]
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(ROOT), timeout=RUN_TIMEOUT)
        if proc.returncode != 0 or not binary.is_file():
            raise GateError("harness compile failed (rc=%d): %s"
                            % (proc.returncode, (proc.stdout + proc.stderr)[-1200:]))
        run = subprocess.run([str(binary)], capture_output=True, text=True,
                             cwd=str(ROOT), timeout=RUN_TIMEOUT)
        return run.stdout, run.returncode


def parse_output(raw: str, returncode: int) -> Tuple[int, int]:
    text = raw.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.strip() for line in text.split("\n")]
    begins = [line for line in lines if line.startswith(BEGIN + " ")]
    ends = [line for line in lines if line == END]
    summaries = [SUMMARY_RE.match(line) for line in lines
                 if line.startswith(SUMMARY + " ")]
    summaries = [m for m in summaries if m]
    checks = [CHECK_RE.match(line) for line in lines
              if line.startswith("os.check ")]
    checks = [m for m in checks if m]
    if len(begins) != 1 or len(ends) != 1 or len(summaries) != 1:
        raise GateError("expected one begin/end/summary marker, got %d/%d/%d"
                        % (len(begins), len(ends), len(summaries)))
    count = int(summaries[0].group(1))
    fails = int(summaries[0].group(2))
    names = [m.group(1) for m in checks]
    if len(names) != len(set(names)):
        raise GateError("duplicate os.check name in harness output")
    failing = [m.group(1) for m in checks if m.group(2) == "FAIL"]
    if count != len(checks):
        raise GateError("summary checks=%d but %d check lines (false green)"
                        % (count, len(checks)))
    if fails != len(failing):
        raise GateError("summary fail=%d but %d FAIL lines (false green)"
                        % (fails, len(failing)))
    if len(checks) < MIN_CHECKS:
        raise GateError("vacuous harness: %d checks, need >= %d"
                        % (len(checks), MIN_CHECKS))
    if returncode != 0 or failing:
        raise GateError("harness red: rc=%d, failing checks: %s"
                        % (returncode, ", ".join(failing[:8]) or "(summary only)"))
    return len(checks), len(failing)


def run_gate(data: Dict[str, Any], src: Dict[str, str], cc: str) -> Tuple[int, int]:
    problems = audit_wiring(src)
    if problems:
        raise GateError("wiring audit failed: " + "; ".join(problems))
    decl = extract_block(src["command.h"], DECL_BEGIN, DECL_END)
    impl = extract_block(src["os_commands.c"], IMPL_BEGIN, IMPL_END)
    harness = render_harness(data, decl, impl)
    raw, rc = compile_and_run(harness, cc)
    return parse_output(raw, rc)


def self_test(cc: str) -> int:
    """Red canaries: each mutation must make the gate fail, never silently pass."""
    canaries = 0
    data = load_fixture(FIXTURE)
    src = read_sources()

    # 1. Stale value: a baseline that no longer matches emitted output is red.
    stale = copy.deepcopy(data)
    stale["cases"][0]["fields"][0]["expected"] = "os.pm.tasks=999"
    try:
        run_gate(stale, src, cc)
    except GateError:
        canaries += 1
    else:
        raise GateError("stale-value canary went green")

    # 2. Missing accessor: unavailable must never report the old cached value.
    missing = copy.deepcopy(data)
    missing["cases"][0]["fields"][0]["available"] = False
    try:
        run_gate(missing, src, cc)
    except GateError:
        canaries += 1
    else:
        raise GateError("missing-accessor canary went green")

    # 3. False green: a summary that disagrees with its checks is rejected.
    raw = (BEGIN + " tag=canary\n"
           "os.check tautology ok\n"
           "os.check tautology ok\n" +
           SUMMARY + " checks=1 fail=0\n" + END + "\n")
    try:
        parse_output(raw, 0)
    except GateError:
        canaries += 1
    else:
        raise GateError("false-green canary went green")

    # 4. Wiring canary: removing the dispatcher arm must be caught.
    broken = dict(src)
    broken["command.c"] = src["command.c"].replace(
        'shell_text_equals_ignore_case(argv[0], "os")',
        'shell_text_equals_ignore_case(argv[0], "zz")', 1)
    try:
        run_gate(data, broken, cc)
    except GateError:
        canaries += 1
    else:
        raise GateError("wiring canary went green")

    print("host_os_gate self-test: PASS (%d canaries)" % canaries)
    return canaries


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="host `os` facade pre-gate")
    parser.add_argument("--check", action="store_true", help="verify (default)")
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    parser.add_argument("--cc", default=shutil.which("cc") or shutil.which("gcc") or "gcc")
    parser.add_argument("--self-test", action="store_true",
                        help="run red canaries, no board")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            self_test(args.cc)
            return 0
        data = load_fixture(args.fixture)
        src = read_sources()
        checks, fails = run_gate(data, src, args.cc)
        print("host_os_gate: PASS (%d cases, %d checks, %d failures, tag=%s)"
              % (len(data["cases"]), checks, fails, data["tag"]))
        return 0
    except GateError as exc:
        print("host_os_gate: FAIL (%s)" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
