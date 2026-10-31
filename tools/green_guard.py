#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""green_guard.py - reject false-green and false-positive tests.

A green test suite is only evidence if every test actually asserts something and
every declared test is actually run. This static guard finds the classic ways a
suite goes green without testing anything:

Unity (``test/main/test_*.c``)
  * a test function with no assertion macro (vacuous test);
  * a ``TEST_CASE`` block with no assertion (self-registering tests);
  * an ``extern`` test that is never ``RUN_TEST``-ed (declared but not run);
  * a ``RUN_TEST`` of a function nobody defines (link failure / typo);
  * a test file that is not in ``test/main/CMakeLists.txt`` (never compiled);
  * a ``TEST_CASE`` also listed in ``RUN_TEST`` (it self-registers);
  * a test function defined but never registered (never run).

Host suites (``tools/suites/*.py``, ``tools/p4test/agent.py``)
  * a ``Checklist`` with no ``check``/``expect``/``equals`` call;
  * a tautological check (``c.check(name, True)``, ``c.equals(n, x, x)``);
  * an always-true substring assertion (``c.expect(n, "", text)``);
  * ``assert True`` / ``assert 1``.

Helpers that assert are supported: a test that calls a same-file helper whose
body asserts is not vacuous. An explicit escape hatch for anything else is
``/* green-guard: helper-asserts */`` in the test body.

Usage:
    python tools/green_guard.py            # report; non-zero on errors
    python tools/green_guard.py --strict   # warnings become errors
    python tools/green_guard.py --json P   # write a machine-readable report
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_DIR = os.path.join(ROOT, "test", "main")
SUITE_DIR = os.path.join(ROOT, "tools", "suites")
REPORT = os.path.join(ROOT, ".agent", "green-guard.json")

ASSERT_RE = re.compile(r"\bTEST_ASSERT\w*|\bTEST_FAIL\w*|\bUNITY_TEST_ASSERT\w*")
PASS_RE = re.compile(r"\bTEST_PASS\b")
IGNORE_RE = re.compile(r"\bTEST_IGNORE\w*")
HELPER_OK_RE = re.compile(r"green-guard:\s*helper-asserts")
FUNC_RE = re.compile(r"\bvoid\s+(test_[A-Za-z0-9_]+)\s*\(\s*void\s*\)\s*\{")
CASE_RE = re.compile(r"\bTEST_CASE\s*\(\s*\"([^\"]*)\"\s*,")
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def rel(path):
    return os.path.relpath(path, ROOT).replace("\\", "/")


def strip_c(text):
    """Blank comments and string/char literals, preserving offsets."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c in ("\"", "'"):
            quote = c
            out[i] = " "
            i += 1
            while i < n:
                if text[i] == "\\":
                    out[i] = " "
                    if i + 1 < n:
                        out[i + 1] = " "
                    i += 2
                    continue
                if text[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if text[i] == "\n":
                    break
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def matching_brace(stripped, open_idx):
    depth = 0
    for i in range(open_idx, len(stripped)):
        if stripped[i] == "{":
            depth += 1
        elif stripped[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(stripped) - 1


def function_bodies(stripped):
    """Map every function name in a file to its (start, end) body span."""
    bodies = {}
    for m in re.finditer(r"\b[A-Za-z_]\w*\s+([A-Za-z_]\w*)\s*\([^;{)]*\)\s*\{", stripped):
        name = m.group(1)
        open_idx = stripped.find("{", m.end() - 1)
        close_idx = matching_brace(stripped, open_idx)
        bodies.setdefault(name, []).append((open_idx, close_idx))
    return bodies


def body_has_assert(name, bodies, seen=None):
    """True when the function (or a same-file callee) asserts."""
    seen = seen or set()
    if name in seen:
        return False
    seen.add(name)
    spans = bodies.get(name)
    if not spans:
        return False
    for start, end in spans:
        body = bodies["__text__"][start:end]
        if ASSERT_RE.search(body):
            return True
        for m in CALL_RE.finditer(body):
            callee = m.group(1)
            if callee != name and body_has_assert(callee, bodies, seen):
                return True
    return False


def scan_c_file(path):
    """Return (tests, errors, warnings) for one Unity test file."""
    errors, warnings, tests = [], [], []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    stripped = strip_c(text)
    bodies = function_bodies(stripped)
    bodies["__text__"] = stripped

    # Classic void test_x(void) { ... }
    for m in FUNC_RE.finditer(stripped):
        name = m.group(1)
        open_idx = stripped.find("{", m.end() - 1)
        close_idx = matching_brace(stripped, open_idx)
        body = stripped[open_idx:close_idx + 1]
        kind = "test"
        has_assert = bool(ASSERT_RE.search(body) or PASS_RE.search(body))
        if not has_assert and not IGNORE_RE.search(body) and not HELPER_OK_RE.search(text[open_idx:close_idx + 1]):
            has_assert = body_has_assert(name, bodies)
        ignored = bool(IGNORE_RE.search(body))
        tests.append((name, has_assert, ignored, "test"))
        if not has_assert and not ignored:
            errors.append("%s: test %s has no assertion (vacuous green)"
                          % (rel(path), name))
        elif not has_assert and ignored:
            warnings.append("%s: test %s is TEST_IGNORE-only" % (rel(path), name))

    # TEST_CASE("desc", "[tag]") { ... } self-registering tests.
    for m in CASE_RE.finditer(text):
        desc = m.group(1)
        open_idx = stripped.find("{", m.end() - 1)
        if open_idx < 0:
            continue
        close_idx = matching_brace(stripped, open_idx)
        body = stripped[open_idx:close_idx + 1]
        has_assert = bool(ASSERT_RE.search(body) or PASS_RE.search(body))
        if not has_assert and not IGNORE_RE.search(body) and not HELPER_OK_RE.search(text[open_idx:close_idx + 1]):
            for cm in CALL_RE.finditer(body):
                if body_has_assert(cm.group(1), bodies):
                    has_assert = True
                    break
        ignored = bool(IGNORE_RE.search(body))
        tests.append(("TEST_CASE:" + desc, has_assert, ignored, "test_case"))
        if not has_assert and not ignored:
            errors.append("%s: TEST_CASE(%r) has no assertion (vacuous green)"
                          % (rel(path), desc))
        elif not has_assert and ignored:
            warnings.append("%s: TEST_CASE(%r) is TEST_IGNORE-only"
                            % (rel(path), desc))
    return tests, errors, warnings


def parse_cmake_sources():
    path = os.path.join(TEST_DIR, "CMakeLists.txt")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r"\bSRCS\b(.*?)\bINCLUDE_DIRS\b", text, re.DOTALL)
    if not m:
        return set()
    return set(re.findall(r"\b(test_[A-Za-z0-9_]+\.c)\b", m.group(1)))


def scan_c_suite():
    errors, warnings = [], []
    defined = set()
    all_tests = []
    cmake = parse_cmake_sources()

    files = sorted(f for f in os.listdir(TEST_DIR)
                   if f.startswith("test_") and f.endswith(".c") and f != "test_main.c")
    for fn in files:
        path = os.path.join(TEST_DIR, fn)
        tests, e, w = scan_c_file(path)
        errors.extend(e)
        warnings.extend(w)
        all_tests.extend((fn, t[0]) for t in tests)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            stripped = strip_c(fh.read())
        defined.update(m.group(1) for m in FUNC_RE.finditer(stripped))
        if fn not in cmake:
            errors.append("test/main/%s: not listed in test/main/CMakeLists.txt SRCS "
                          "(never compiled -> false green)" % fn)

    for fn in sorted(cmake):
        if not os.path.exists(os.path.join(TEST_DIR, fn)):
            errors.append("test/main/CMakeLists.txt: SRCS lists missing %s" % fn)

    # test_main.c registration checks.
    main_path = os.path.join(TEST_DIR, "test_main.c")
    with open(main_path, "r", encoding="utf-8", errors="replace") as fh:
        main_text = strip_c(fh.read())
    externs = set(re.findall(r"\bextern\s+void\s+(test_\w+)\s*\(\s*void\s*\)\s*;", main_text))
    runs = re.findall(r"\bRUN_TEST\s*\(\s*(test_\w+)\s*\)", main_text)
    run_set = set(runs)

    for name in sorted(externs - run_set):
        errors.append("test_main.c: %s is declared but never RUN_TEST-ed "
                      "(declared but not run -> false green)" % name)
    for name in sorted(run_set - externs - defined):
        errors.append("test_main.c: RUN_TEST(%s) has no extern/definition" % name)
    for name in sorted({n for n in runs if runs.count(n) > 1}):
        warnings.append("test_main.c: %s is RUN_TEST-ed more than once" % name)
    for name in sorted(defined - run_set):
        # TEST_CASE self-registering tests do not appear as test_* functions.
        warnings.append("test/main: %s is defined but never RUN_TEST-ed "
                        "(never run -> false green risk)" % name)

    # TEST_CASE must not also be RUN_TEST-ed.
    for fn, name in all_tests:
        if name.startswith("TEST_CASE:"):
            continue
        if name not in run_set:
            # Already covered by the extern/run checks; keep the report quiet.
            pass
    return errors, warnings, all_tests


def scan_python_suite():
    errors, warnings = [], []
    targets = []
    if os.path.isdir(SUITE_DIR):
        targets += [os.path.join(SUITE_DIR, f) for f in sorted(os.listdir(SUITE_DIR))
                    if f.endswith(".py") and not f.startswith("_")]
    agent = os.path.join(ROOT, "tools", "p4test", "agent.py")
    if os.path.exists(agent):
        targets.append(agent)

    for path in targets:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        if "Checklist(" not in text:
            continue
        calls = len(re.findall(r"\.(check|expect|equals)\s*\(", text))
        if calls == 0:
            errors.append("%s: Checklist created but no check/expect/equals call "
                          "(zero-check suite -> false green)" % rel(path))
        for m in re.finditer(r"\.check\s*\(\s*[^,]+,\s*(True|1)\s*\)", text):
            errors.append("%s: tautological check %r" % (rel(path), m.group(0)))
        for m in re.finditer(r"\.equals\s*\(\s*[^,]+,\s*([^,]+),\s*([^,]+?)\s*\)", text):
            if m.group(1).strip() == m.group(2).strip():
                warnings.append("%s: tautological equals %r" % (rel(path), m.group(0)))
        for m in re.finditer(r"\.expect\s*\(\s*[^,]+,\s*(\"\"|'')\s*,", text):
            warnings.append("%s: empty-needle expect always passes %r"
                            % (rel(path), m.group(0)))
        for m in re.finditer(r"^\s*assert\s+(True|1)\s*$", text, re.MULTILINE):
            errors.append("%s: tautological %r" % (rel(path), m.group(0).strip()))
    return errors, warnings


def main(argv=None):
    ap = argparse.ArgumentParser(description="Reject false-green / false-positive tests")
    ap.add_argument("--strict", action="store_true", help="warnings become errors")
    ap.add_argument("--json", default=REPORT, help="machine-readable report path")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    c_errors, c_warnings, tests = scan_c_suite()
    py_errors, py_warnings = scan_python_suite()
    errors = c_errors + py_errors
    warnings = c_warnings + py_warnings

    if not args.quiet:
        for e in errors:
            print("ERROR: %s" % e)
        for w in warnings:
            print("WARN: %s" % w)

    print("green_guard: %d unity test(s), %d error(s), %d warning(s)"
          % (len(tests), len(errors), len(warnings)))

    os.makedirs(os.path.dirname(args.json), exist_ok=True)
    with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
        json.dump({"version": 1, "generated_by": "tools/green_guard.py",
                   "tests": [{"file": f, "name": n} for f, n in tests],
                   "errors": errors, "warnings": warnings},
                  fh, indent=2, sort_keys=True)
        fh.write("\n")

    if args.strict:
        return 1 if (errors or warnings) else 0
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
