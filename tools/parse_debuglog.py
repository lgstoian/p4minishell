#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""parse_debuglog.py - host-side parser for `debug save` exports.

Reads a debug-log export (txt, csv, or json -- format auto-detected from
the file extension, overridable with --format) as produced by the firmware
`debug save [file] [txt|csv|json]` command (default sd:/DEBUG.LOG; pull it
with tools/pull.py), and prints a severity summary plus a chronological
table. Mirrors the firmware tint rule: an entry containing ERROR reads as
error, WARN as warning, anything else as info.

Usage: python tools/parse_debuglog.py DEBUG.LOG [--format txt|csv|json]
"""
import argparse
import csv
import json
import os
import sys


def load_txt(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.startswith("["):
                end = line.find("]")
                if end > 1 and line[1:end].strip().isdigit():
                    rows.append((int(line[1:end]), line[end + 1:].strip()))
                    continue
            rows.append((len(rows), line))
    return rows


def load_csv(path):
    rows = []
    with open(path, "r", encoding="utf-8", newline="", errors="replace") as fh:
        reader = csv.reader(fh)
        header = next(reader, None)
        if header is None:
            return rows
        for line in reader:
            if len(line) < 2 or not line[0].strip().isdigit():
                continue
            rows.append((int(line[0]), line[1]))
    return rows


def load_json(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        doc = json.load(fh)
    entries = doc.get("log", [])
    return [(i, str(e)) for i, e in enumerate(entries)]


def severity(entry):
    if "ERROR" in entry:
        return "error"
    if "WARN" in entry:
        return "warning"
    return "info"


def split_tag(entry):
    tag, sep, message = entry.partition(": ")
    if sep:
        return tag.strip(), message.strip()
    return "-", entry


def main():
    ap = argparse.ArgumentParser(description="Parse a `debug save` export")
    ap.add_argument("file", help="exported log (see `debug save`)")
    ap.add_argument("--format", choices=("txt", "csv", "json"), default=None)
    args = ap.parse_args()

    fmt = args.format
    if fmt is None:
        ext = os.path.splitext(args.file)[1].lower().lstrip(".")
        fmt = ext if ext in ("txt", "csv", "json") else "txt"
    try:
        rows = {"txt": load_txt, "csv": load_csv, "json": load_json}[fmt](args.file)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print("parse_debuglog: cannot read %s (%s)" % (args.file, exc))
        return 1

    counts = {"error": 0, "warning": 0, "info": 0}
    table = []
    for index, entry in rows:
        level = severity(entry)
        counts[level] += 1
        tag, message = split_tag(entry)
        table.append((index, level, tag, message))

    print("debuglog: %d entries (%d error, %d warning, %d info)" % (
        len(table), counts["error"], counts["warning"], counts["info"]))
    print("%5s  %-7s  %-12s  %s" % ("idx", "level", "tag", "message"))
    for index, level, tag, message in table:
        print("%5d  %-7s  %-12.12s  %s" % (index, level, tag, message[:100]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
