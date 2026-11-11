#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_unit_run.py - build and run the host (linux-target) unit suite.

The host lane compiles the shared `test/main/test_*.c` sources with ESP-IDF's
"linux" target and runs them as a native executable, so pure logic is checked
on every PR without a board. It is a **pre-gate**: a host pass is not hardware
sign-off (`harness.md` §0 still governs).

Usage:
  python tools/host_unit_run.py              # build (if needed) and run
  python tools/host_unit_run.py --build      # force a rebuild first
  python tools/host_unit_run.py --no-build   # run the existing executable
  python tools/host_unit_run.py --static     # manifest/drift checks only
  python tools/host_unit_run.py -v           # echo the raw run log

Exit code is non-zero on a failed case, a missing completion marker, a case
count that disagrees with tools/host_test_manifest.json, or a static-gate
finding.

The build needs a POSIX host (Linux, macOS or WSL2) with ESP-IDF v5.5.5
sourced, because the "linux" target is a preview host target.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_DIR = os.path.join(ROOT, "test", "host")
ELF = os.path.join(HOST_DIR, "build", "p4minishell_host_tests.elf")
MANIFEST = os.path.join(ROOT, "tools", "host_test_manifest.json")
TEST_DIR = os.path.join(ROOT, "test", "main")

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# Same contract as tools/unit_run.py: Unity's own summary line per block.
SUMMARY = re.compile(
    r"(\d+)\s+Tests?\s+(\d+)\s+Failures?\s+(\d+)\s+Ignored", re.IGNORECASE)
DONE = "=== All tests completed ==="
RUN_TIMEOUT = 180


def load_manifest() -> dict:
    with open(MANIFEST, "r", encoding="utf-8-sig") as fh:
        return json.load(fh)


def build() -> int:
    if not os.environ.get("IDF_PATH"):
        print("host_unit_run: IDF_PATH is not set; source export.sh first",
              file=sys.stderr)
        return 1
    print("host_unit_run: building test/host (idf.py build)", flush=True)
    proc = subprocess.run(["idf.py", "build"], cwd=HOST_DIR)
    return proc.returncode


def run_elf(verbose: bool) -> tuple:
    if not os.path.isfile(ELF):
        print("host_unit_run: %s is missing; build it first" % ELF,
              file=sys.stderr)
        return None
    try:
        proc = subprocess.run([ELF], cwd=HOST_DIR, timeout=RUN_TIMEOUT,
                              capture_output=True)
    except subprocess.TimeoutExpired:
        print("host_unit_run: run timed out after %ds" % RUN_TIMEOUT,
              file=sys.stderr)
        return None
    except OSError as exc:
        print("host_unit_run: cannot execute %s here (%s)." % (ELF, exc),
              file=sys.stderr)
        print("host_unit_run: the host lane builds a POSIX binary - run this "
              "from Linux, macOS or WSL2.", file=sys.stderr)
        return None
    text = ANSI.sub("", (proc.stdout + proc.stderr).decode("utf-8",
                                                           errors="replace"))
    if verbose:
        print(text, flush=True)
    return proc.returncode, text


def report(rc: int, text: str) -> int:
    lines = text.splitlines()
    summaries = [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
                 for m in (SUMMARY.search(l) for l in lines) if m]
    tests = sum(s[0] for s in summaries)
    failures = sum(s[1] for s in summaries)
    ignored = sum(s[2] for s in summaries)
    completed = DONE in text

    for l in lines:
        if SUMMARY.search(l):
            print("  " + l.strip()[:140])
    if failures or rc != 0:
        for l in lines:
            if "FAIL" in l.upper() and "Failures" not in l:
                print("  " + l.strip()[:160])

    print("HOST TOTAL tests: %d  failures: %d  ignored: %d"
          % (tests, failures, ignored))
    print("HOST completed: %s" % ("yes" if completed else "NO"))
    ok = completed and failures == 0 and tests > 0 and rc == 0
    print("HOST UNIT RESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


def static_checks() -> int:
    """Manifest/drift gate: the file list is the single source of truth."""
    if not os.path.isfile(MANIFEST):
        print("host_unit_run: %s is missing" % MANIFEST, file=sys.stderr)
        return 1
    man = load_manifest()
    problems = []

    for name in man.get("tests", []):
        path = os.path.join(TEST_DIR, name)
        if not os.path.isfile(path):
            problems.append("listed test source is missing: %s" % name)

    on_disk = sorted(f for f in os.listdir(TEST_DIR)
                     if f.startswith("test_") and f.endswith(".c"))
    excluded = man.get("excluded", {})
    unlisted = [f for f in on_disk
                if f not in man.get("tests", []) and f not in excluded]
    if man.get("phase", 0) >= 1:
        problems.extend("test source is neither included nor excluded: %s" % f
                        for f in unlisted)
    else:
        for f in unlisted:
            print("host_unit_run: phase-0 note: %s not yet classified" % f)

    for f, reason in excluded.items():
        if not str(reason).strip():
            problems.append("exclusion has no reason: %s" % f)

    if os.path.isfile(ELF) and man.get("expected_cases"):
        got = run_elf(False)
        if got:
            m = [x for x in (SUMMARY.search(l) for l in got[1].splitlines())
                 if x]
            total = sum(int(x.group(1)) for x in m)
            want = int(man["expected_cases"])
            if total != want:
                problems.append("case count drifted: manifest %d, run %d "
                                "(update test/host/main/CMakeLists.txt and "
                                "host_main.c together)" % (want, total))

    for p in problems:
        print("HOST STATIC FAIL: " + p)
    print("HOST STATIC %s" % ("OK" if not problems else "FAIL"))
    return 0 if not problems else 1


def main() -> int:
    argv = sys.argv[1:]
    verbose = "-v" in argv or "--verbose" in argv
    want_build = "--build" in argv
    no_build = "--no-build" in argv
    static = "--static" in argv

    if static:
        return static_checks()

    if not no_build and (want_build or not os.path.isfile(ELF)):
        rc = build()
        if rc != 0:
            print("HOST UNIT RESULT FAIL (build)")
            return rc

    got = run_elf(verbose)
    if got is None:
        print("HOST UNIT RESULT FAIL (no run)")
        return 1
    return report(got[0], got[1])


if __name__ == "__main__":
    sys.exit(main())
