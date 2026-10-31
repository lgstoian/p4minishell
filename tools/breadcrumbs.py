#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""breadcrumbs.py - the machine-readable navigation map for AI agents.

Builds the codebase breadcrumb map from ``comment-index.json``:

* ``breadcrumbs.json`` - the machine-readable map (modules, layers, tests,
  rules, and a per-file breadcrumb trail).
* ``BREADCRUMBS.md``   - the human-readable map an agent reads to orient itself.

Every source file under a component gets a breadcrumb line of the form

    components/shell/shell.c :: module=shell layer=shell domain=core
        tests=test/main/test_shell_parser.c
        rules=ai-context.md#shell-core-rules

so an agent can state exactly where it is (and what governs it) before it
changes anything. The tags themselves live in the headers; see
``docs/comment-tags.md``.

Usage:
    python tools/breadcrumbs.py                 # validate only (default)
    python tools/breadcrumbs.py --write         # refresh both artifacts
    python tools/breadcrumbs.py --for PATH      # print one file's breadcrumb
    python tools/breadcrumbs.py --layer shell   # list one layer's modules
"""
from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX = os.path.join(ROOT, "comment-index.json")
OUT_JSON = os.path.join(ROOT, "breadcrumbs.json")
OUT_MD = os.path.join(ROOT, "BREADCRUMBS.md")

ENTRY_POINTS = [
    "AGENTS.md",
    "ai-context.md",
    "architecture.md",
    "documentation.md",
    "SDK.md",
    "command.md",
    "batch.md",
    "ABI.md",
    "harness.md",
    "test/README.md",
    "tools/README.md",
    "PORTING.md",
    "schematics.md",
    "SECURITY.md",
    "release.md",
    "bugs.md",
    "roadmap.md",
    "changelog.md",
]

DOC_OWNER = {
    "shell": "documentation.md#shell",
    "storage": "documentation.md#storage",
    "batch": "batch.md",
    "command": "command.md",
    "applib": "SDK.md",
    "editor": "editor.md",
    "gfx": "documentation.md#graphics",
    "header": "documentation.md#header",
    "tui": "documentation.md#tui",
    "markdown": "documentation.md#markdown",
    "networking": "documentation.md#networking",
    "db": "documentation.md#database",
    "alarm": "documentation.md#alarms",
    "pim": "documentation.md#pim",
    "filetype": "documentation.md#filetype",
    "font": "documentation.md#font",
    "modal": "documentation.md#modal",
    "boot": "documentation.md#boot-scripting",
    "main": "architecture.md",
}


def rel(path):
    return os.path.relpath(path, ROOT).replace("\\", "/")


def load_index():
    with open(INDEX, "r", encoding="utf-8") as fh:
        return json.load(fh)


def module_files(module):
    """Every .c/.h file belonging to a module (sorted, repo-relative)."""
    base = os.path.join(ROOT, module["path"])
    out = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in ("managed_components", "build")]
        for fn in filenames:
            if fn.endswith((".c", ".h")):
                out.append(rel(os.path.join(dirpath, fn)))
    return sorted(out)


def breadcrumb_for(path, modules_by_dir):
    """Return the breadcrumb trail for a repo-relative source path."""
    parts = path.replace("\\", "/").split("/")
    comp = None
    if parts[0] == "components" and len(parts) > 1:
        comp = parts[1]
    elif parts[0] == "main":
        comp = "main"
    if comp is None or comp not in modules_by_dir:
        return None
    m = modules_by_dir[comp]
    trail = ["module=%s" % comp, "layer=%s" % m["layer"], "domain=%s" % m["domain"]]
    if m.get("status"):
        trail.append("status=%s" % m["status"])
    if m.get("tests"):
        trail.append("tests=%s" % ",".join(m["tests"]))
    if m.get("rules"):
        trail.append("rules=%s" % ",".join(m["rules"]))
    return " :: ".join(trail)


def validate(index):
    errors = []
    modules = index.get("modules", {})
    seen_paths = {}
    for comp, m in modules.items():
        if comp in seen_paths:
            errors.append("duplicate module %r" % comp)
        seen_paths[comp] = m.get("path")
        if m.get("layer") not in index.get("layers", []):
            errors.append("%s: unknown layer %r" % (comp, m.get("layer")))
        for t in m.get("tests", []):
            if not os.path.exists(os.path.join(ROOT, t)):
                errors.append("%s: test %r missing" % (comp, t))
        for r in m.get("rules", []):
            doc = r.split("#", 1)[0]
            if doc and not os.path.exists(os.path.join(ROOT, doc)):
                errors.append("%s: rule doc %r missing" % (comp, doc))
    return errors


def build(index):
    modules = index["modules"]
    modules_by_dir = dict(modules)
    breadcrumbs = {}
    for comp, m in modules.items():
        for f in module_files(m):
            bc = breadcrumb_for(f, modules_by_dir)
            if bc:
                breadcrumbs[f] = bc
    return {
        "version": 1,
        "generated_by": "tools/breadcrumbs.py",
        "layer_order": index.get("layers", []),
        "entry_points": ENTRY_POINTS,
        "doc_owner": DOC_OWNER,
        "modules": modules,
        "breadcrumbs": breadcrumbs,
    }


def render_md(index, data):
    modules = index["modules"]
    lines = []
    lines.append("# P4MiniShell - Agent breadcrumbs")
    lines.append("")
    lines.append("Generated by `tools/breadcrumbs.py` from `comment-index.json`.")
    lines.append("Read this (and `AGENTS.md`) before changing anything; it is the")
    lines.append("map of *where things are* and *what governs them*. The tags live in")
    lines.append("each component's primary header - see `docs/comment-tags.md`.")
    lines.append("")
    lines.append("## Start here")
    lines.append("")
    for ep in ENTRY_POINTS:
        lines.append("- [%s](%s)" % (ep, ep))
    lines.append("")
    lines.append("## Layer order")
    lines.append("")
    lines.append("`" + " -> ".join(index.get("layers", [])) + "`")
    lines.append("")
    lines.append("Dependencies flow one way down this chain; the registration tables")
    lines.append("invert the only upward edges. Never add a trampoline.")
    lines.append("")
    lines.append("## Modules")
    lines.append("")
    lines.append("| Module | Layer | Domain | Provides | Tests | Rules | Path |")
    lines.append("|--------|-------|--------|----------|-------|-------|------|")
    for comp in sorted(modules):
        m = modules[comp]
        tests = ", ".join("[t](%s)" % t for t in m.get("tests", [])) or "-"
        rules = ", ".join("`%s`" % r for r in m.get("rules", [])) or "-"
        lines.append("| %s | %s | %s | %s | %s | %s | `%s` |"
                     % (comp, m.get("layer", ""), m.get("domain", ""),
                        m.get("provides", ""), tests, rules, m.get("path", "")))
    lines.append("")
    lines.append("## Per-file breadcrumbs")
    lines.append("")
    lines.append("Every source file's trail (also in `breadcrumbs.json`):")
    lines.append("")
    for f in sorted(data["breadcrumbs"]):
        lines.append("- `%s` :: %s" % (f, data["breadcrumbs"][f]))
    lines.append("")
    lines.append("## Using the trail")
    lines.append("")
    lines.append("```powershell")
    lines.append("python tools/breadcrumbs.py --for components/shell/shell.c")
    lines.append("python tools/breadcrumbs.py --layer shell")
    lines.append("python tools/breadcrumbs.py --write")
    lines.append("```")
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build/validate the agent breadcrumb map")
    ap.add_argument("--write", action="store_true", help="write breadcrumbs.json + BREADCRUMBS.md")
    ap.add_argument("--for", dest="for_path", default=None, help="print one file's breadcrumb")
    ap.add_argument("--layer", default=None, help="list the modules in one layer")
    ap.add_argument("--json", default=OUT_JSON, help="JSON output path")
    args = ap.parse_args(argv)

    if not os.path.exists(INDEX):
        print("ERROR: %s missing - run `python tools/comment_index.py --write` first" % rel(INDEX))
        return 1

    index = load_index()
    errors = validate(index)

    data = build(index)
    md_text = render_md(index, data)

    # The committed artifacts must match the tree; a stale map is an error.
    if not args.write:
        checks = [
            (args.json, json.dumps(data, indent=2, sort_keys=True) + "\n"),
            (OUT_MD, md_text),
        ]
        for path, expected in checks:
            if not os.path.exists(path):
                errors.append("%s missing; run `python tools/breadcrumbs.py --write`"
                              % rel(path))
                continue
            with open(path, "r", encoding="utf-8") as fh:
                if fh.read() != expected:
                    errors.append("%s is stale; run `python tools/breadcrumbs.py --write`"
                                  % rel(path))

    for e in errors:
        print("ERROR: %s" % e)

    if args.for_path:
        bc = breadcrumb_for(args.for_path.replace("\\", "/"), index["modules"])
        print(bc if bc else "no breadcrumb for %s" % args.for_path)
        return 1 if errors else 0

    if args.layer:
        for comp in sorted(index["modules"]):
            if index["modules"][comp].get("layer") == args.layer:
                print("  %-16s %s" % (comp, index["modules"][comp].get("provides", "")))
        return 1 if errors else 0

    print("breadcrumbs: %d modules, %d file breadcrumbs, %d error(s)"
          % (len(index["modules"]), len(data["breadcrumbs"]), len(errors)))

    if args.write and not errors:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
            fh.write("\n")
        with open(OUT_MD, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(md_text)
        print("wrote %s and %s" % (rel(args.json), rel(OUT_MD)))

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
