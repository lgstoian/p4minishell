#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""htmlcheck.py - host-side validator for `markdown export` output.

Checks the files the writerdeck `markdown export ... html|print|text`
formats produce, so the harness can verify *content* (not just that a file
appeared). Uses only the standard library; a browser is not required.

Used by tools/suites/s07_data.py and runnable directly:

    python tools/htmlcheck.py DOC.HTML      # HTML page
    python tools/htmlcheck.py DOC.PRN       # paginated print layout
    python tools/htmlcheck.py DOC.TXT       # plain text

Exports:
    check_html(text, title=None) -> list[str]   problems ([] == OK)
    check_print(text, title=None) -> list[str]
    check_text(text) -> list[str]
"""
from __future__ import annotations

import sys
from html.parser import HTMLParser
from typing import List, Optional

# Elements that never have a closing tag.
_VOID = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr",
}
_UNSAFE_SCHEMES = ("javascript:", "data:", "vbscript:", "file:")


class _Balance(HTMLParser):
    """Track tag balance and unsafe attribute URLs for a well-formedness read."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.stack: List[str] = []
        self.problems: List[str] = []

    def _attrs(self, tag: str, attrs) -> None:
        for name, value in attrs:
            if value and any(value.strip().lower().startswith(s)
                             for s in _UNSAFE_SCHEMES):
                self.problems.append("unsafe URL in <%s %s=%r>" % (tag, name, value[:48]))

    def handle_starttag(self, tag, attrs):
        self._attrs(tag, attrs)
        if tag not in _VOID:
            self.stack.append(tag)

    def handle_startendtag(self, tag, attrs):
        self._attrs(tag, attrs)

    def handle_endtag(self, tag):
        if tag in _VOID:
            return
        if not self.stack:
            self.problems.append("stray </%s>" % tag)
            return
        if self.stack[-1] == tag:
            self.stack.pop()
            return
        if tag in self.stack:
            while self.stack and self.stack[-1] != tag:
                self.problems.append("unclosed <%s>" % self.stack.pop())
            if self.stack:
                self.stack.pop()
        else:
            self.problems.append("unmatched </%s>" % tag)


def check_html(text: str, title: Optional[str] = None) -> List[str]:
    """Validate an exported standalone HTML page. Returns a problem list."""
    problems: List[str] = []

    if "<!DOCTYPE html>" not in text:
        problems.append("missing <!DOCTYPE html>")
    if "<meta charset=\"utf-8\">" not in text:
        problems.append("missing utf-8 charset meta")
    if "<title>" not in text or "</title>" not in text:
        problems.append("missing <title>")
    if title and ("<title>%s</title>" % title) not in text:
        problems.append("title %r not in <title>" % title)
    if "<main>" not in text or "</main>" not in text:
        problems.append("missing <main> wrapper")

    parser = _Balance()
    parser.feed(text)
    parser.close()
    unclosed = [t for t in parser.stack if t not in ("html", "body")]
    if unclosed:
        problems.append("unclosed tags: %s" % ", ".join(unclosed))
    problems.extend(parser.problems)
    return problems


def check_print(text: str, title: Optional[str] = None) -> List[str]:
    """Validate a paginated print layout. Returns a problem list."""
    problems: List[str] = []

    if "Page 1" not in text:
        problems.append("missing 'Page 1' header")
    if "\f" not in text:
        problems.append("no form feed between pages")
    if title and title not in text:
        problems.append("title %r not in page header" % title)
    return problems


def check_text(text: str) -> List[str]:
    """Validate a plain-text (ANSI-stripped) export. Returns a problem list."""
    problems: List[str] = []
    if "\x1b" in text:
        problems.append("contains ANSI escape (0x1b); should be stripped")
    return problems


def _kind_for(path: str) -> str:
    low = path.lower()
    if low.endswith((".html", ".htm")):
        return "html"
    if low.endswith((".prn", ".print")):
        return "print"
    return "text"


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    kind = _kind_for(path)
    if kind == "html":
        problems = check_html(text)
    elif kind == "print":
        problems = check_print(text)
    else:
        problems = check_text(text)
    if problems:
        print("FAIL %s (%s):" % (path, kind))
        for p in problems:
            print("  - %s" % p)
        return 1
    print("OK %s (%s)" % (path, kind))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
