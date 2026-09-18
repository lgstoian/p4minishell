# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Lightweight PASS/FAIL assertions and suite reporting.

Every suite builds one :class:`Checklist`. A check never raises on a plain
assertion failure; it records the result and keeps going so one run reports
every problem. Only a device panic (handled in the session) aborts a suite.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import List, Optional, Sequence


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""


@dataclass
class SuiteResult:
    name: str
    passed: int
    failed: int
    skipped: int = 0
    duration: float = 0.0
    error: Optional[str] = None

    @property
    def ok(self) -> bool:
        return self.failed == 0 and self.error is None

    @property
    def total(self) -> int:
        return self.passed + self.failed + self.skipped


class Checklist:
    """Collect named checks, print a compact table, and expose a verdict."""

    def __init__(self, name: str, verbose: bool = True):
        self.name = name
        self.verbose = verbose
        self.checks: List[Check] = []
        self.notes: List[str] = []
        self.started = time.time()

    # -- recording -------------------------------------------------------
    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append(Check(name, bool(ok), detail))
        if self.verbose:
            tag = "PASS" if ok else "FAIL"
            suffix = (" (%s)" % detail) if detail and not ok else ""
            print("  [%s] %s%s" % (tag, name, suffix))
            if not ok and detail:
                print("        %s" % detail.replace("\n", "\n        ")[:1200])
        return bool(ok)

    def expect(self, name: str, needle: str, text: str, want: bool = True) -> bool:
        """Assert ``needle`` is (or is not) present in ``text``."""
        present = needle in text
        ok = present == want
        detail = ""
        if not ok:
            detail = "%r %s in output. Tail:\n%s" % (
                needle, "found" if present else "MISSING", text[-600:])
        return self.check(name, ok, detail)

    def equals(self, name: str, got, want) -> bool:
        return self.check(name, got == want, "got %r want %r" % (got, want))

    def note(self, text: str) -> None:
        self.notes.append(text)
        if self.verbose:
            print("  [note] %s" % text)

    # -- reporting -------------------------------------------------------
    @property
    def passed(self) -> int:
        return sum(1 for c in self.checks if c.ok)

    @property
    def failed(self) -> int:
        return sum(1 for c in self.checks if not c.ok)

    @property
    def total(self) -> int:
        return len(self.checks)

    @property
    def ok(self) -> bool:
        return self.failed == 0

    def result(self, error: Optional[str] = None) -> SuiteResult:
        return SuiteResult(self.name, self.passed, self.failed,
                           duration=time.time() - self.started, error=error)

    def raise_if_failed(self) -> None:
        if not self.ok:
            raise AssertionError("%s: %d check(s) failed" % (self.name, self.failed))

    def print_report(self) -> None:
        status = "PASS" if self.ok else "FAIL"
        print("\n%s: %s (%d/%d checks)" % (self.name, status, self.passed, self.total))
        if not self.ok:
            for c in self.checks:
                if not c.ok:
                    print("  - %s: %s" % (c.name, c.detail[:500]))
        print("RESULT %s" % ("OK" if self.ok else "FAIL"))

    def summary_text(self) -> str:
        return "%s: %d passed, %d failed, %d total" % (
            self.name, self.passed, self.failed, self.total)


def first_mismatch(got: Sequence, want: Sequence) -> Optional[str]:
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return "index %d: got %r want %r" % (i, a, b)
    if len(got) != len(want):
        return "length %d != %d" % (len(got), len(want))
    return None
