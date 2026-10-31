#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""comment_index.py - extract and validate the machine-readable source tags.

Reads the ``@module``/``@layer``/``@domain``/``@provides``/``@status``/``@test``/
``@hard-rule`` tags from every component's primary header, combines them with the
component's ``CMakeLists.txt`` ``REQUIRES`` list (the single source of truth for
dependencies), and writes ``comment-index.json``.

The vocabulary and the rules are documented in ``docs/comment-tags.md``.

Usage:
    python tools/comment_index.py            # validate only (default)
    python tools/comment_index.py --write    # validate + refresh comment-index.json
    python tools/comment_index.py --json P   # write to an alternate path

Exit code is non-zero when any error is found. Warnings never fail the gate.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LAYERS = ["main", "command", "batch", "storage", "shell", "leaf", "app"]
DOMAINS = ["core", "fs", "ui", "net", "media", "data", "crypto", "platform", "app"]
STATUSES = ["stable", "experimental", "deprecated"]

# component -> primary header relative to components/<name>/
PRIMARY_HEADER = {
    "board_caps": "include/board_caps.h",
    "camera": "include/camera.h",
    "cas": "include/cas.h",
    "certs": "include/certs.h",
    "chat": "include/chat.h",
    "epub": "include/epub.h",
    "event_bus": "include/event_bus.h",
    "imagefmt": "include/imagefmt.h",
    "imu": "include/imu.h",
    "jsengine": "include/jsengine.h",
    "json": "include/json.h",
    "math_disp": "include/math_disp.h",
    "p4heap": "include/p4heap.h",
    "portal": "include/portal.h",
    "power_monitor": "include/power_monitor.h",
    "rss": "include/rss.h",
    "state_machine": "include/state_machine.h",
    "strutil": "include/strutil.h",
    "swgcm": "include/sw_gcm.h",
    "uitest": "include/ui_test.h",
    "zip": "include/zip.h",
}

# In-tree components (everything under components/, excluding vendored dirs).
IN_TREE = set()
for entry in sorted(os.listdir(os.path.join(ROOT, "components"))):
    if os.path.isdir(os.path.join(ROOT, "components", entry)):
        IN_TREE.add(entry)

# Forbidden dependency edges, derived from ai-context.md §Module Layering Rules.
# (component, forbidden-requirement)
FORBIDDEN_EDGES = [
    ("shell", "command"), ("shell", "batch"),
    ("shell", "networking"), ("shell", "usb"), ("shell", "c6ota"),
    ("batch", "command"),
    ("storage", "command"), ("storage", "batch"),
    ("applib", "command"), ("applib", "batch"), ("applib", "networking"),
    ("db", "command"), ("db", "batch"),
    ("alarm", "command"), ("alarm", "batch"),
]

# Forbidden header includes (component, header basename).
FORBIDDEN_INCLUDES = [
    ("shell", "command.h"), ("shell", "batch.h"),
    ("shell", "networking.h"), ("shell", "usb.h"), ("shell", "c6ota.h"),
    ("shell", "bluetooth.h"),
    ("batch", "command.h"),
    ("storage", "command.h"), ("storage", "batch.h"),
    ("applib", "command.h"), ("applib", "batch.h"), ("applib", "networking.h"),
    ("db", "command.h"), ("db", "batch.h"),
    ("alarm", "command.h"), ("alarm", "batch.h"),
]

TAG_RE = re.compile(r"^@\s*(\w[\w-]*)\s*(.*)$")
FILE_DOC_RE = re.compile(r"/\*\*(.*?)\*/", re.DOTALL)


def rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace("\\", "/")


def read_text(path: str) -> str:
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", errors="replace")


def leading_doc_block(text: str):
    """Return the first /** ... */ block, or None."""
    m = FILE_DOC_RE.search(text)
    return m.group(1) if m else None


def parse_tags(block):
    """Extract the machine tags from a doc block (dict of tag -> raw value)."""
    tags = {}
    if not block:
        return tags
    for line in block.splitlines():
        line = line.strip()
        if line.startswith("*"):
            line = line[1:].strip()
        m = TAG_RE.match(line)
        if not m:
            continue
        name, value = m.group(1), m.group(2).strip()
        if name in ("module", "layer", "domain", "provides", "status", "test", "hard-rule"):
            tags[name] = value
    return tags


def parse_requires(cmake_path: str):
    """Return the REQUIRES list from a component CMakeLists.txt."""
    text = read_text(cmake_path)
    m = re.search(r"\bREQUIRES\b(.*?)\)", text, re.DOTALL)
    if not m:
        return []
    body = m.group(1)
    # Strip comments and split on whitespace.
    body = re.sub(r"#[^\n]*", " ", body)
    return [tok for tok in re.split(r"\s+", body.strip()) if tok]


def slugify(heading: str) -> str:
    """GitHub-style anchor for a markdown heading."""
    text = heading.strip().lower()
    text = re.sub(r"[^\w\s-]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", "-", text)
    return text


def doc_anchors(path: str):
    """Return the set of anchors in a markdown document."""
    anchors = set()
    try:
        text = read_text(path)
    except OSError:
        return anchors
    for line in text.splitlines():
        m = re.match(r"^#{1,6}\s+(.*)$", line)
        if m:
            anchors.add(slugify(m.group(1)))
    return anchors


def find_ai_comments():
    """Yield (path, lineno, line) for every `// AI:` comment in the tree."""
    hits = []
    roots = ["components", "main", "boards", "test"]
    for top in roots:
        base = os.path.join(ROOT, top)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in ("managed_components", "build")]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    lines = read_text(p).splitlines()
                except OSError:
                    continue
                for i, line in enumerate(lines, 1):
                    if "// AI:" in line or "/* AI:" in line:
                        hits.append((rel(p), i, line.strip()))
    return hits


def check_forbidden_includes(errors):
    for comp, header in FORBIDDEN_INCLUDES:
        base = os.path.join(ROOT, "components", comp)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in ("managed_components",)]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, fn)
                for i, line in enumerate(read_text(p).splitlines(), 1):
                    if re.search(r'#\s*include\s+["<]%s"[>]' % re.escape(header), line):
                        errors.append("%s:%d includes forbidden %s"
                                      % (rel(p), i, header))


def collect_modules():
    """Return (modules, errors, warnings)."""
    modules = {}
    errors = []
    warnings = []

    # main/ is not a component but is part of the map.
    modules["main"] = {
        "path": "main",
        "header": None,
        "layer": "main",
        "domain": "platform",
        "provides": "app entry, boot sequencing, LVGL event callbacks, host bridges",
        "status": "stable",
        "tests": [],
        "rules": ["ai-context.md#module-layering-rules"],
        "requires": [],
    }

    for comp in sorted(IN_TREE):
        cdir = os.path.join(ROOT, "components", comp)
        header_rel = PRIMARY_HEADER.get(comp, comp + ".h")
        header_path = os.path.join(cdir, header_rel)
        cmake_path = os.path.join(cdir, "CMakeLists.txt")

        if not os.path.exists(header_path):
            errors.append("components/%s: primary header %s not found"
                          % (comp, header_rel))
            continue

        block = leading_doc_block(read_text(header_path))
        tags = parse_tags(block)
        if not tags:
            errors.append("components/%s: primary header %s has no machine tags"
                          % (comp, header_rel))
            continue

        module = tags.get("module", "")
        if module != comp:
            errors.append("components/%s: @module %r does not match directory"
                          % (comp, module))

        layer = tags.get("layer", "")
        if layer not in LAYERS:
            errors.append("components/%s: unknown @layer %r" % (comp, layer))

        domain = tags.get("domain", "")
        if domain not in DOMAINS:
            errors.append("components/%s: unknown @domain %r" % (comp, domain))

        status = tags.get("status", "")
        if status not in STATUSES:
            errors.append("components/%s: unknown @status %r" % (comp, status))

        tests = [t.strip() for t in tags.get("test", "").split(",") if t.strip()]
        for t in tests:
            if not os.path.exists(os.path.join(ROOT, t)):
                errors.append("components/%s: @test path %r does not exist" % (comp, t))
        if not tests:
            warnings.append("components/%s: no @test line" % comp)

        rules = [r.strip() for r in tags.get("hard-rule", "").split(",") if r.strip()]
        for r in rules:
            if "#" in r:
                doc, anchor = r.split("#", 1)
            else:
                doc, anchor = r, None
            doc_path = os.path.join(ROOT, doc)
            if not os.path.exists(doc_path):
                errors.append("components/%s: @hard-rule doc %r does not exist" % (comp, doc))
            elif anchor:
                anchors = doc_anchors(doc_path)
                if anchor not in anchors:
                    errors.append("components/%s: @hard-rule anchor %r not in %s"
                                  % (comp, anchor, doc))

        requires = parse_requires(cmake_path) if os.path.exists(cmake_path) else []

        modules[comp] = {
            "path": "components/%s" % comp,
            "header": rel(header_path),
            "layer": layer,
            "domain": domain,
            "provides": tags.get("provides", ""),
            "status": status,
            "tests": tests,
            "rules": rules,
            "requires": requires,
        }

    # Forbidden edges.
    for comp, forbidden in FORBIDDEN_EDGES:
        entry = modules.get(comp)
        if not entry:
            continue
        reqs = entry.get("requires", [])
        if forbidden in reqs:
            errors.append("components/%s: forbidden requirement %r (ai-context.md)"
                          % (comp, forbidden))
        if "main" in reqs:
            errors.append("components/%s: may not require 'main'" % comp)

    # main is required by nobody.
    for comp, entry in modules.items():
        if comp == "main":
            continue
        if "main" in entry.get("requires", []):
            errors.append("components/%s: may not require 'main'" % comp)

    check_forbidden_includes(errors)

    # // AI: sweep.
    for path, lineno, line in find_ai_comments():
        errors.append("%s:%d: %s" % (path, lineno, line))

    return modules, errors, warnings


def build_manifest(modules):
    return {
        "version": 1,
        "generated_by": "tools/comment_index.py",
        "layers": LAYERS,
        "domains": DOMAINS,
        "statuses": STATUSES,
        "modules": {k: modules[k] for k in sorted(modules)},
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract/validate machine-readable comment tags")
    ap.add_argument("--write", action="store_true",
                    help="write comment-index.json (default: validate only)")
    ap.add_argument("--json", default=os.path.join(ROOT, "comment-index.json"),
                    help="manifest output path")
    ap.add_argument("--quiet", action="store_true", help="only print the summary")
    args = ap.parse_args(argv)

    modules, errors, warnings = collect_modules()
    manifest = build_manifest(modules)

    # The committed manifest must match the tree; a stale manifest makes the
    # breadcrumb map lie, so it is an error, not a warning.
    if not args.write:
        if not os.path.exists(args.json):
            errors.append("%s missing; run `python tools/comment_index.py --write`"
                          % rel(args.json))
        else:
            try:
                with open(args.json, "r", encoding="utf-8") as fh:
                    current = json.load(fh)
            except (OSError, ValueError) as exc:
                errors.append("cannot read %s: %s" % (rel(args.json), exc))
            else:
                if current != manifest:
                    errors.append("%s is stale; run `python tools/comment_index.py --write`"
                                  % rel(args.json))

    if not args.quiet:
        for comp in sorted(modules):
            m = modules[comp]
            print("  %-16s layer=%-8s domain=%-8s requires=%d tests=%d"
                  % (comp, m["layer"], m["domain"], len(m["requires"]), len(m["tests"])))

    for w in warnings:
        print("WARN: %s" % w)
    for e in errors:
        print("ERROR: %s" % e)

    print("comment_index: %d modules, %d error(s), %d warning(s)"
          % (len(modules), len(errors), len(warnings)))

    if args.write and not errors:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(manifest, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote %s" % rel(args.json))

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
