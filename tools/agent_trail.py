#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""agent_trail.py - a machine-readable trail of what an agent changed and why.

Each session appends one JSON line to ``.agent/trail.jsonl`` (git-ignored). The
trail is deliberately small: timestamp, agent/session, task, status, touched
files, the breadcrumb modules involved, a summary, and the next step. The next
agent (or a human) reads it with ``list``/``summary``/``export-md`` instead of
re-deriving the context from ``git log``.

The trail is *activity*, not a second changelog: keep entries short and point at
the real change (the commit/diff is the record of what the code does).

Usage:
    python tools/agent_trail.py record --task "..." --files git --summary "..."
    python tools/agent_trail.py list [--limit 20]
    python tools/agent_trail.py summary
    python tools/agent_trail.py export-md [--out .agent/AGENT_TRAIL.md]
    python tools/agent_trail.py prune --keep 200
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AGENT_DIR = os.path.join(ROOT, ".agent")
TRAIL = os.path.join(AGENT_DIR, "trail.jsonl")
INDEX = os.path.join(ROOT, "comment-index.json")


def _now():
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _module_for(path):
    p = path.replace("\\", "/").lstrip("./")
    parts = p.split("/")
    if parts[0] == "components" and len(parts) > 1:
        return parts[1]
    if parts[0] == "main":
        return "main"
    if parts[0] == "test":
        return "test"
    if parts[0] == "tools":
        return "tools"
    if parts[0] == "boards":
        return "boards"
    return parts[0] if parts else "?"


def _git_files():
    try:
        out = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=ROOT, capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    files = []
    for line in out.splitlines():
        if len(line) > 3:
            files.append(line[3:].strip())
    return files


def _known_modules():
    try:
        with open(INDEX, "r", encoding="utf-8") as fh:
            return set(json.load(fh).get("modules", {}))
    except (OSError, ValueError):
        return set()


def _read_trail():
    if not os.path.exists(TRAIL):
        return []
    entries = []
    with open(TRAIL, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                entries.append(json.loads(line))
            except ValueError:
                continue
    return entries


def _append(entry):
    os.makedirs(AGENT_DIR, exist_ok=True)
    with open(TRAIL, "a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(entry, sort_keys=True) + "\n")


def cmd_record(args):
    if args.files == "git":
        files = _git_files()
    else:
        files = [f.strip() for f in (args.files or "").split(",") if f.strip()]

    if args.breadcrumb:
        modules = [m.strip() for m in args.breadcrumb.split(",") if m.strip()]
        modules = [_module_for(m) if "/" in m or "\\" in m else m for m in modules]
    else:
        modules = sorted({_module_for(f) for f in files})

    known = _known_modules()
    unknown = [m for m in modules if known and m not in known and m not in ("test", "tools", "boards")]
    for m in unknown:
        print("WARN: unknown module %r (not in comment-index.json)" % m)

    entry = {
        "ts": _now(),
        "agent": args.agent or os.environ.get("P4_AGENT", "agent"),
        "session": args.session or os.environ.get("OPENCODE_SESSION", ""),
        "task": args.task,
        "status": args.status,
        "modules": modules,
        "files": files,
        "summary": args.summary or "",
        "next": args.next or "",
    }
    _append(entry)
    print("trail: %s | %s | %s | %d file(s) | %s"
          % (entry["ts"], entry["agent"], entry["task"],
             len(files), ",".join(modules) or "-"))
    return 0


def cmd_list(args):
    entries = _read_trail()
    if not entries:
        print("no trail entries")
        return 0
    for e in entries[-args.limit:]:
        print("%s  %-8s %-10s %s%s"
              % (e.get("ts", "?"), e.get("agent", "?"),
                 e.get("status", "?"), e.get("task", ""),
                 ("  [" + ",".join(e.get("modules", [])) + "]")
                 if e.get("modules") else ""))
        if e.get("summary"):
            print("    %s" % e["summary"])
        if e.get("next"):
            print("    next: %s" % e["next"])
    return 0


def cmd_summary(args):
    entries = _read_trail()
    if not entries:
        print("no trail entries")
        return 0
    by_module = {}
    for e in entries:
        for m in e.get("modules", []) or ["?"]:
            by_module.setdefault(m, []).append(e)
    for m in sorted(by_module):
        print("%s (%d)" % (m, len(by_module[m])))
        for e in by_module[m][-3:]:
            print("  %s %s" % (e.get("ts", "?"), e.get("task", "")))
    return 0


def cmd_show(args):
    entries = _read_trail()
    if not entries:
        print("no trail entries")
        return 0
    print(json.dumps(entries[-1], indent=2, sort_keys=True))
    return 0


def cmd_export(args):
    entries = _read_trail()
    out = args.out or os.path.join(AGENT_DIR, "AGENT_TRAIL.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    lines = ["# Agent trail", "",
             "Generated by `tools/agent_trail.py` from `.agent/trail.jsonl`.",
             "Newest entries last.", ""]
    for e in entries:
        lines.append("## %s - %s" % (e.get("ts", "?"), e.get("task", "")))
        lines.append("")
        lines.append("- agent: `%s`%s" % (
            e.get("agent", "?"),
            (" / session `%s`" % e["session"]) if e.get("session") else ""))
        lines.append("- status: `%s`" % e.get("status", "?"))
        if e.get("modules"):
            lines.append("- modules: `%s`" % "`, `".join(e["modules"]))
        if e.get("files"):
            lines.append("- files: `%s`" % "`, `".join(e["files"]))
        if e.get("summary"):
            lines.append("- summary: %s" % e["summary"])
        if e.get("next"):
            lines.append("- next: %s" % e["next"])
        lines.append("")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("wrote %s (%d entries)" % (os.path.relpath(out, ROOT), len(entries)))
    return 0


def cmd_prune(args):
    entries = _read_trail()
    keep = entries[-args.keep:]
    os.makedirs(AGENT_DIR, exist_ok=True)
    with open(TRAIL, "w", encoding="utf-8", newline="\n") as fh:
        for e in keep:
            fh.write(json.dumps(e, sort_keys=True) + "\n")
    print("trail: kept %d of %d entries" % (len(keep), len(entries)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Record/read the agent activity trail")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("record", help="append one entry")
    r.add_argument("--task", required=True)
    r.add_argument("--files", default="git",
                   help="comma-separated paths, or 'git' for git status (default)")
    r.add_argument("--summary", default="")
    r.add_argument("--breadcrumb", default="",
                   help="comma-separated module names or paths (default: derived from --files)")
    r.add_argument("--status", default="done", choices=("done", "wip", "blocked"))
    r.add_argument("--next", default="")
    r.add_argument("--agent", default=None)
    r.add_argument("--session", default=None)
    r.set_defaults(func=cmd_record)

    l = sub.add_parser("list", help="list recent entries")
    l.add_argument("--limit", type=int, default=20)
    l.set_defaults(func=cmd_list)

    s = sub.add_parser("summary", help="group entries by module")
    s.set_defaults(func=cmd_summary)

    sh = sub.add_parser("show", help="show the last entry as JSON")
    sh.set_defaults(func=cmd_show)

    e = sub.add_parser("export-md", help="write .agent/AGENT_TRAIL.md")
    e.add_argument("--out", default=None)
    e.set_defaults(func=cmd_export)

    p = sub.add_parser("prune", help="keep only the last N entries")
    p.add_argument("--keep", type=int, default=200)
    p.set_defaults(func=cmd_prune)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
